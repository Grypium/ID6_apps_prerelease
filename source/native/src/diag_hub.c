/* diag_hub.c - included by collector.c when built with -DDIAG_HUB.
 *
 * One shared, read-only diagnostic worker for the gauge apps. The same binary
 * is installed as BoostDeck's boostdeck-collector-qnx and ShiftDeck's
 * shift-deck-qnx, so the official plugins launch it unchanged:
 *   BoostDeck: --host IP --session      -> client "boost" (NBG3 frames, UDP 42860)
 *   ShiftDeck: --host IP                -> client "shift" (drive frames, UDP 42863)
 *   PerfTimers: --host IP --client timer -> client "timer" (drive frames, UDP 42865)
 *
 * Rules, so the gauges never crowd out the car's own systems or other tools:
 *   - Only one process talks to the car. The first instance binds the hub
 *     control port and owns the single gateway connection; any other instance
 *     only sends "still wanted" heartbeats and never touches the gateway. If
 *     the owner's screen closes, a remaining instance takes over.
 *   - Read services only (22 ReadDataByIdentifier, OBD mode 01), one request in
 *     flight, at least min_gap_ms between requests, and the gap stretches when
 *     the DME answers slowly. Several values are read per request.
 *   - Holds the shared loopback lock while connected and gives it up within one
 *     loop (<= ~50 ms) when a one-shot tool (fault scan, map switch) asks for it,
 *     then stays off for yield_ms.
 *   - Every guard_period_ms it reads the DME's active session (22 F186). Any
 *     session other than default means another tester (MHD, ISTA, dealer tool)
 *     is working on the engine, so it disconnects and sends nothing for
 *     guard_pause_ms. Errors and timeouts back off exponentially.
 *   - Each instance exits when its plugin (parent) goes away; plugins stop it
 *     when their gauge screen is hidden.
 */
#include "drive_frame.h"

enum {
    hub_port = 42864,          /* loopback control port: heartbeats */
    heartbeat_ms = 300,
    subscriber_ttl_ms = 1000,
    min_gap_ms = 40,           /* hard ceiling: 25 requests per second */
    boost_fast_ms = 150,
    boost_slow_ms = 1000,
    trans_ms = 2000,
    shift_ms = 80,
    timer_ms = 50,             /* speed for the performance timers */
    timer_port = 42865,
    guard_period_ms = 2000,
    yield_ms = 3000,
    request_timeout_ms = 300,
    max_backoff_ms = 60000
};

enum { C_BOOST = 1, C_SHIFT = 2, C_TIMER = 4 };

static unsigned guard_pause_ms = 30000;
static unsigned lock_port = DIAGNOSTIC_PORT;
static int trace_hub = 0;

/* --- shared lock (same protocol as diagnostic_lease.h, port configurable for tests) */
static int hub_reserve(void) {
    struct sockaddr_in a;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)lock_port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0 || bind(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int someone_waiting(int lock) {
    char b[16];
    int seen = 0;
    while (recv(lock, b, sizeof b, 0) > 0) seen = 1;
    return seen;
}

static int udp_socket(unsigned port, int bind_it) {
    struct sockaddr_in a;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) { close(fd); return -1; }
    if (bind_it) {
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_port = htons((uint16_t)port);
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return -1; }
    }
    return fd;
}

static void to_loopback(struct sockaddr_in *a, unsigned port) {
    memset(a, 0, sizeof *a);
    a->sin_family = AF_INET;
    a->sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &a->sin_addr);
}

static void nap(uint64_t ms) {
    if (ms > 1000) ms = 1000;
    if (running && ms) poll(0, 0, (int)ms);
}

/* --- gateway session state (owner only) */
static const char *gw_host;
static unsigned gw_port = 6801;
static int gw = -1, lock = -1;
static uint64_t last_request, paused_until, backoff_ms, avg_latency;
static unsigned requests_sent;

static void disconnect(void) {
    release_gateway(&gw, &lock);
}

static void pause_for(uint64_t ms, const char *why) {
    disconnect();
    paused_until = bd_mono_ms() + ms;
    if (trace_hub) fprintf(stderr, "hub: pause %llu ms (%s)\n", (unsigned long long)ms, why);
}

/* Ready to send one request now? Connects lazily and enforces the gap. */
static int may_send(void) {
    uint64_t now = bd_mono_ms(), gap;
    if (now < paused_until) return 0;
    if (lock >= 0 && someone_waiting(lock)) { pause_for(yield_ms, "lock requested"); return 0; }
    gap = min_gap_ms;
    if (avg_latency * 2 > gap) gap = avg_latency * 2;
    if (now < last_request + gap) return 0;
    if (gw < 0) {
        lock = hub_reserve();
        if (lock < 0) { paused_until = now + 500; return 0; } /* a one-shot is running */
        gw = connect_gw(gw_host, gw_port);
        if (gw < 0) { pause_for(backoff_ms ? backoff_ms : 1000, "connect failed"); return 0; }
    }
    return 1;
}

static void failed(void) {
    backoff_ms = backoff_ms ? backoff_ms * 2 : 1000;
    if (backoff_ms > max_backoff_ms) backoff_ms = max_backoff_ms;
    pause_for(backoff_ms, "no answer");
}

/* One read request. Returns reply length, -2 for a negative answer, or -1
 * (connection dropped; already paused). */
static int hub_read(unsigned ecu, const unsigned char *uds, unsigned n, const unsigned char *prefix,
                    unsigned prefix_n, unsigned char *reply, unsigned size, unsigned *negative) {
    uint64_t start = bd_mono_ms(), took;
    int rc;
    last_request = start;
    ++requests_sent;
    rc = exchange(gw, ecu, uds, n, prefix, prefix_n, reply, size, request_timeout_ms, negative);
    took = bd_mono_ms() - start;
    last_request = bd_mono_ms();
    avg_latency = (avg_latency * 7 + took) >> 3;
    if (trace_hub)
        fprintf(stderr, "hub: req ecu=%02X sid=%02X len=%u rc=%d nrc=%02X took=%llu\n",
                ecu, uds[0], n, rc, rc == -2 ? *negative : 0, (unsigned long long)took);
    if (rc == -1) { failed(); return -1; }
    if (rc == -2 && (*negative == 0x21 || *negative == 0x22 || *negative == 0x7f)) {
        failed(); /* busy / wrong conditions / wrong session: someone else is working */
        return -2;
    }
    backoff_ms = 0;
    return rc;
}

/* --- signals */
static const unsigned fast_dids[] = {0x4205, 0x5889, 0x4a49, 0x56d7, 0x586f};
static const int fast_slot[] = {BD_BOOST, BD_AFR, BD_TIMING, BD_RAIL, BD_OIL};
static const unsigned slow_dids[] = {0x4201, 0x580f, 0x4300};
static const int slow_slot[] = {-1, BD_IAT, BD_COOLANT}; /* 4201 = ambient, used for boost */

static int multi_ok = 1; /* several DIDs per request until the DME refuses */
static bd_frame frame;
static uint64_t observed[BD_COUNT];
static double ambient;
static uint64_t ambient_time;
static int have_ambient;

static unsigned did_size(unsigned did) { return did == 0x580f ? 1u : 2u; }

static void store(unsigned did, int slot, const unsigned char *p, unsigned n) {
    double v;
    if (slot >= 0) frame.valid &= ~(1u << slot);
    if (!bd_decode_did(did, p, n, &v)) return;
    if (did == 0x4201) { ambient = v; ambient_time = bd_mono_ms(); have_ambient = 1; return; }
    if (did == 0x4205) {
        if (!have_ambient || bd_mono_ms() - ambient_time > 1800) return;
        v = (v - ambient) * .145037738; /* kPa absolute - ambient -> gauge psi */
    }
    if (did == 0x5889) v *= 14.7; /* lambda -> gasoline-equivalent AFR */
    frame.value[slot] = v;
    observed[slot] = bd_mono_ms();
    frame.valid |= 1u << slot;
}

/* Reads a DID group from the DME: one multi-DID request, or one per DID if
 * the DME does not take several. Returns 0 if the connection was lost. */
static int read_group(const unsigned *dids, const int *slots, unsigned count) {
    unsigned char req[1 + 2 * 8], prefix[3], reply[256];
    unsigned i, negative;
    int n;
    if (multi_ok && count > 1) {
        unsigned at = 1;
        req[0] = 0x22;
        for (i = 0; i < count; ++i) { req[1+2*i] = (unsigned char)(dids[i] >> 8); req[2+2*i] = (unsigned char)dids[i]; }
        prefix[0] = 0x62; prefix[1] = req[1]; prefix[2] = req[2];
        n = hub_read(0x12, req, 1 + 2 * count, prefix, 3, reply, sizeof reply, &negative);
        if (n == -1) return 0;
        if (n > 0) {
            for (i = 0; i < count; ++i) {
                unsigned size = did_size(dids[i]);
                if (at + 2 + size > (unsigned)n || reply[at] != (dids[i] >> 8) || reply[at+1] != (dids[i] & 255)) break;
                store(dids[i], slots[i], reply + at + 2, size);
                at += 2 + size;
            }
            if (i == count && at == (unsigned)n) return 1;
        }
        if (n == -2 && paused_until > bd_mono_ms()) return 1; /* busy: backing off */
        multi_ok = 0; /* refused or unexpected layout: one DID per request from now on */
        if (trace_hub) fprintf(stderr, "hub: multi-DID read not accepted; single reads\n");
        return 1;
    }
    for (i = 0; i < count; ++i) {
        while (running && !may_send()) {
            if (bd_mono_ms() < paused_until) return 1;
            nap(5);
        }
        req[0] = 0x22; req[1] = (unsigned char)(dids[i] >> 8); req[2] = (unsigned char)dids[i];
        prefix[0] = 0x62; prefix[1] = req[1]; prefix[2] = req[2];
        n = hub_read(0x12, req, 3, prefix, 3, reply, sizeof reply, &negative);
        if (n == -1) return 0;
        if (n == 3 + (int)did_size(dids[i])) store(dids[i], slots[i], reply + 3, did_size(dids[i]));
        else if (slots[i] >= 0) frame.valid &= ~(1u << slots[i]);
    }
    return 1;
}

static int read_trans(void) {
    static const unsigned char req[] = {0x22, 0xda, 0x12}, prefix[] = {0x62, 0xda, 0x12};
    unsigned char reply[16];
    unsigned negative;
    int n = hub_read(0x18, req, 3, prefix, 3, reply, sizeof reply, &negative);
    if (n == -1) return 0;
    if (n == 5) store(0xda12, BD_TRANS, reply + 3, 2);
    else frame.valid &= ~(1u << BD_TRANS);
    return 1;
}

static int multi_pid_ok = 1;
static struct drive_frame drive;

static int read_shift(void) {
    unsigned char reply[16];
    unsigned negative;
    int n;
    drive.rpm = drive.kph = -1;
    if (multi_pid_ok) {
        static const unsigned char req[] = {0x01, 0x0c, 0x0d}, prefix[] = {0x41};
        n = hub_read(0x12, req, 3, prefix, 1, reply, sizeof reply, &negative);
        if (n == -1) return 0;
        if (n == 6 && reply[1] == 0x0c && reply[4] == 0x0d) {
            drive.rpm = (int)(((unsigned)reply[2] * 256 + reply[3]) >> 2);
            drive.kph = reply[5];
        } else if (!(n == -2 && paused_until > bd_mono_ms())) {
            multi_pid_ok = 0;
            if (trace_hub) fprintf(stderr, "hub: combined OBD read not accepted; separate reads\n");
        }
    } else {
        static const unsigned char rq[] = {0x01, 0x0c}, rp[] = {0x41, 0x0c};
        static const unsigned char kq[] = {0x01, 0x0d}, kp[] = {0x41, 0x0d};
        n = hub_read(0x12, rq, 2, rp, 2, reply, sizeof reply, &negative);
        if (n == -1) return 0;
        if (n == 4) drive.rpm = (int)(((unsigned)reply[2] * 256 + reply[3]) >> 2);
        while (running && !may_send()) {
            if (bd_mono_ms() < paused_until) return 1;
            nap(5);
        }
        n = hub_read(0x12, kq, 2, kp, 2, reply, sizeof reply, &negative);
        if (n == -1) return 0;
        if (n == 3) drive.kph = reply[2];
    }
    drive.sampled_ms = bd_mono_ms();
    return 1;
}

/* 22 F186 -> 62 F1 86 <session>. 01 = default. Anything else: another tester. */
static int guard_supported = 1;
static int check_session(void) {
    static const unsigned char req[] = {0x22, 0xf1, 0x86}, prefix[] = {0x62, 0xf1, 0x86};
    unsigned char reply[8];
    unsigned negative;
    int n = hub_read(0x12, req, 3, prefix, 3, reply, sizeof reply, &negative);
    if (n == -1) return 0;
    if (n == 4 && reply[3] != 0x01) pause_for(guard_pause_ms, "DME not in default session");
    else if (n == -2 && negative == 0x31) guard_supported = 0;
    return 1;
}

static void send_boost(int sock, struct sockaddr_in *dest) {
    char packet[BD_MAX_PACKET];
    uint64_t now = bd_mono_ms();
    unsigned i;
    int len;
    frame.seq++;
    frame.sent_ms = now;
    for (i = 0; i < BD_COUNT; ++i) {
        uint64_t age = observed[i] ? now - observed[i] : 999999;
        frame.age_ms[i] = age > 999999 ? 999999 : (unsigned)age;
    }
    len = bd_encode(&frame, packet, sizeof packet);
    if (len > 0) sendto(sock, packet, (size_t)len, 0, (struct sockaddr *)dest, sizeof *dest);
}

int main(int argc, char **argv) {
    unsigned boost_port = BD_UDP_PORT, drive_port = DRIVE_PORT, control_port = hub_port, timer_out = timer_port;
    unsigned cycles = 0, done = 0, i;
    int me = C_SHIFT, control = -1, out, explicit_client = 0;
    pid_t parent = getppid();
    struct sockaddr_in boost_dest, drive_dest, timer_dest, control_dest;
    uint64_t seen[5] = {0, 0, 0, 0, 0}, next_beat = 0, due_fast = 0, due_slow = 0, due_trans = 0,
             due_shift = 0, due_guard = 0, last_boost_frame = 0;

    for (i = 1; i < (unsigned)argc; ++i) {
        if (!strcmp(argv[i], "--host") && i + 1 < (unsigned)argc && !gw_host) gw_host = argv[++i];
        else if (!strcmp(argv[i], "--session")) { if (!explicit_client) me = C_BOOST; }
        else if (!strcmp(argv[i], "--client") && i + 1 < (unsigned)argc) {
            ++i; explicit_client = 1;
            if (!strcmp(argv[i], "boost")) me = C_BOOST;
            else if (!strcmp(argv[i], "shift")) me = C_SHIFT;
            else if (!strcmp(argv[i], "timer")) me = C_TIMER;
            else return 2;
        }
        else if (!strcmp(argv[i], "--port") && i + 1 < (unsigned)argc) gw_port = number(argv[++i], 1, 65535);
        else if (!strcmp(argv[i], "--udp-port") && i + 1 < (unsigned)argc) {
            unsigned p = number(argv[++i], 1, 65535);
            if (me == C_BOOST) boost_port = p; else if (me == C_TIMER) timer_out = p; else drive_port = p;
        }
        else if (!strcmp(argv[i], "--boost-port") && i + 1 < (unsigned)argc) boost_port = number(argv[++i], 1, 65535);
        else if (!strcmp(argv[i], "--drive-port") && i + 1 < (unsigned)argc) drive_port = number(argv[++i], 1, 65535);
        else if (!strcmp(argv[i], "--timer-port") && i + 1 < (unsigned)argc) timer_out = number(argv[++i], 1, 65535);
        else if (!strcmp(argv[i], "--hub-port") && i + 1 < (unsigned)argc) control_port = number(argv[++i], 1, 65535);
        else if (!strcmp(argv[i], "--lock-port") && i + 1 < (unsigned)argc) lock_port = number(argv[++i], 1, 65535);
        else if (!strcmp(argv[i], "--guard-pause-ms") && i + 1 < (unsigned)argc) guard_pause_ms = number(argv[++i], 100, 600000);
        else if (!strcmp(argv[i], "--cycles") && i + 1 < (unsigned)argc) cycles = number(argv[++i], 1, 1000000);
        else if (!strcmp(argv[i], "--trace")) trace_hub = 1;
        else if (!strcmp(argv[i], "--no-trans")) {}
        else {
            fprintf(stderr, "Usage: %s --host IPv4 [--session | --client boost|shift|timer] [--port 6801] [--trace]\n", argv[0]);
            return 2;
        }
    }
    if (!gw_host || parent <= 1) return 2;
    signal(SIGINT, stop); signal(SIGTERM, stop); signal(SIGPIPE, SIG_IGN);
    out = udp_socket(0, 0);
    if (out < 0) return 1;
    to_loopback(&boost_dest, boost_port);
    to_loopback(&drive_dest, drive_port);
    to_loopback(&timer_dest, timer_out);
    to_loopback(&control_dest, control_port);
    memset(&frame, 0, sizeof frame);
    memset(&drive, 0, sizeof drive);

    while (running && getppid() == parent) {
        uint64_t now = bd_mono_ms();
        int wanted, sent = 0;
        if (control < 0) control = udp_socket(control_port, 1); /* become the owner if free */
        if (control < 0) {
            /* Not the owner: only say we are still wanted. Never touch the car. */
            if (now >= next_beat) {
                char beat = (char)me;
                sendto(out, &beat, 1, 0, (struct sockaddr *)&control_dest, sizeof control_dest);
                next_beat = now + heartbeat_ms;
            }
            nap(50);
            continue;
        }
        {   /* owner: collect heartbeats */
            char b[16];
            ssize_t k;
            while ((k = recv(control, b, sizeof b, 0)) > 0)
                if (b[0] == C_BOOST || b[0] == C_SHIFT || b[0] == C_TIMER) seen[(int)b[0]] = now;
        }
        wanted = me;
        if (seen[C_BOOST] && now - seen[C_BOOST] < subscriber_ttl_ms) wanted |= C_BOOST;
        if (seen[C_SHIFT] && now - seen[C_SHIFT] < subscriber_ttl_ms) wanted |= C_SHIFT;
        if (seen[C_TIMER] && now - seen[C_TIMER] < subscriber_ttl_ms) wanted |= C_TIMER;

        if (now >= paused_until && gw < 0 && lock < 0 && trace_hub && paused_until)
            fprintf(stderr, "hub: resuming\n");
        if (may_send()) {
            /* Earliest-due task first; guard and shift take precedence on ties. */
            if (guard_supported && now >= due_guard) {
                due_guard = now + guard_period_ms;
                sent = check_session();
            } else if ((wanted & (C_SHIFT | C_TIMER)) && now >= due_shift) {
                due_shift = now + ((wanted & C_TIMER) ? timer_ms : shift_ms);
                if ((sent = read_shift()) && now >= paused_until) {
                    if (wanted & C_SHIFT)
                        sendto(out, &drive, sizeof drive, 0, (struct sockaddr *)&drive_dest, sizeof drive_dest);
                    if (wanted & C_TIMER)
                        sendto(out, &drive, sizeof drive, 0, (struct sockaddr *)&timer_dest, sizeof timer_dest);
                }
            } else if ((wanted & C_BOOST) && now >= due_slow) {
                due_slow = now + boost_slow_ms;
                sent = read_group(slow_dids, slow_slot, 3);
            } else if ((wanted & C_BOOST) && now >= due_trans) {
                due_trans = now + trans_ms;
                sent = read_trans();
            } else if ((wanted & C_BOOST) && now >= due_fast) {
                due_fast = now + boost_fast_ms;
                sent = read_group(fast_dids, fast_slot, 5);
                if (cycles && ++done >= cycles) running = 0;
            }
            (void)sent;
        }
        if ((wanted & C_BOOST) && now - last_boost_frame >= boost_fast_ms) {
            if (now < paused_until) frame.valid = 0; /* paused: show gauges as unavailable */
            send_boost(out, &boost_dest);
            last_boost_frame = now;
        }
        nap(gw >= 0 ? 5 : 20);
    }
    disconnect();
    if (control >= 0) close(control);
    close(out);
    if (trace_hub) fprintf(stderr, "hub: exit after %u requests\n", requests_sent);
    return 0;
}
