/* diag_hub.c - included by collector.c when built with -DDIAG_HUB.
 *
 * One shared, read-only diagnostic worker for the gauge apps. The same binary
 * is installed as BoostDeck's boostdeck-collector-qnx and ShiftDeck's
 * shift-deck-qnx, so the official plugins launch it unchanged:
 *   BoostDeck: --host IP --session      -> client "boost" (NBG3 frames, UDP 42860)
 *   ShiftDeck: --host IP                -> client "shift" (drive frames, UDP 42863)
 *   PerfTimers: --host IP --client timer -> client "timer" (drive frames, UDP 42865)
 *   EngineTemps: --host IP --client temps -> client "temps" (temps frames, UDP 42866)
 *   rebuilt BoostDeck: --client deck  -> "boost" + deck frames (oil temp, torque,
 *                       power, throttle; UDP 42867)
 *   rebuilt ShiftDeck: --client gear  -> "shift" + gear frames (gear, turbine rpm; UDP 42868)
 *   PerfTimers also gets wheel speeds from the transmission (EGS DA1D).
 *   FuelBlend:  --client fuel  -> tank level (OBD 2F) every 2 s (UDP 42869)
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
#include "temps_frame.h"
#include "hub_frames.h"
#include "most/most_link.h"
#include "most/dsi_client.h"

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
    temps_ms = 1000,           /* oil, coolant, intake air: one request a second */
    trans_stale_ms = 5000,
    deck_ms = 300,             /* torque, throttle (and rpm for power) */
    gear_ms = 200,             /* EGS: gear + turbine speed */
    wheel_ms = 60,             /* EGS: wheel speeds for the timers (EGS answers in ~3 ms) */
    timer_obd_ms = 250,        /* OBD speed, fallback for the timers */
    ethanol_ms = 5000,
    fuel_ms = 2000,
    ethanol_retry_ms = 60000,
    guard_period_ms = 2000,
    yield_ms = 3000,
    request_timeout_ms = 300,
    max_backoff_ms = 60000
};

enum { C_BOOST = 1, C_SHIFT = 2, C_TIMER = 4, C_TEMPS = 8, C_DECK = 16, C_GEAR = 32, C_FUEL = 64, C_LAST = C_FUEL };

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
static const char *gw_host;  /* from gateway-ip.txt: tried first */
static unsigned gw_port = 6801;

/* Gateway discovery, as dealer tools do it: when the configured address can't
   be reached, broadcast the HSFZ vehicle identification request (UDP 6811,
   control word 0x0011, no payload) and use whichever gateway answers
   (DIAGADR10). The gateway picks its 169.254.x.x address itself, so it can
   differ between starts. Read-only: nothing is sent to any ECU. Notable events
   go to a small log on the head unit's RAM disk (no VIN or other reply data). */
static char gw_found[16];            /* last address found by discovery */
static const char *gw_now;           /* address in use: gw_found or gw_host */
static const char *discover_addr = "169.254.255.255";
static unsigned discover_port = 6811;
static int discover_any = 0;         /* tests: accept a non-link-local responder */
static const char *log_path = "/tmp/diag-hub.log";
static uint64_t discover_due, connect_failures, last_logged_failure;
static unsigned log_lines;

static void hub_log(const char *fmt, const char *a, unsigned long long n) {
    struct timespec t;
    char line[160];
    int at, more;
    FILE *f;
    if (!log_path[0] || log_lines >= 40) return; /* at most 40 lines per hub process */
    clock_gettime(CLOCK_REALTIME, &t);
    at = snprintf(line, sizeof line, "%lld: ", (long long)t.tv_sec);
    if (at < 0 || at >= (int)sizeof line) return;
    more = snprintf(line + at, sizeof line - at, fmt, a, n);
    if (more < 0) return;
    at += more;
    if (at > (int)sizeof line - 2) at = (int)sizeof line - 2;
    line[at++] = '\n';
    /* One fwrite of the whole line: the compiler turns single-character writes
       into fputc, which the car's libc import list doesn't otherwise need. */
    f = fopen(log_path, "a");
    if (f) { fwrite(line, 1, (size_t)at, f); fclose(f); }
    ++log_lines;
    if (trace_hub) fwrite(line, 1, (size_t)at, stderr);
}

/* Broadcasts one identification request; on a gateway reply copies its address
   into gw_found and returns 1. Waits up to 1.5 s. */
static int discover_gateway(void) {
    static const unsigned char request[6] = {0, 0, 0, 0, 0, 0x11};
    struct sockaddr_in to, from;
    unsigned char reply[512];
    int fd, on = 1, found = 0;
    uint64_t deadline = bd_mono_ms() + 1500;
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons((uint16_t)discover_port);
    if (inet_pton(AF_INET, discover_addr, &to.sin_addr) != 1) return 0;
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return 0;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof on);
    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0 ||
        sendto(fd, request, sizeof request, 0, (struct sockaddr *)&to, sizeof to) != (ssize_t)sizeof request) {
        close(fd);
        return 0;
    }
    while (!found && running) {
        socklen_t len = sizeof from;
        ssize_t n;
        uint64_t now = bd_mono_ms();
        if (now >= deadline || !ready(fd, POLLIN, deadline)) break;
        n = recvfrom(fd, reply, sizeof reply, 0, (struct sockaddr *)&from, &len);
        if (n < 6 || reply[4] != 0 || reply[5] != 0x11) continue;
        {
            const unsigned char *ip = (const unsigned char *)&from.sin_addr.s_addr;
            ssize_t i;
            int gateway = 0;
            for (i = 6; i + 9 <= n; ++i)
                if (!memcmp(reply + i, "DIAGADR10", 9)) { gateway = 1; break; }
            if (!gateway) continue; /* the head unit itself (DIAGADR63) also answers */
            if (!discover_any && !(ip[0] == 169 && ip[1] == 254)) continue;
            snprintf(gw_found, sizeof gw_found, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
            found = 1;
        }
    }
    close(fd);
    return found;
}

/* Connects to the address in use; on failure tries discovery (at most every 5 s). */
static int connect_gateway(void) {
    int fd;
    uint64_t now;
    if (!gw_now) gw_now = gw_host;
    fd = connect_gw(gw_now, gw_port);
    if (fd >= 0) {
        if (connect_failures) hub_log("connected to %s after %llu failed attempts", gw_now, connect_failures);
        connect_failures = 0;
        return fd;
    }
    ++connect_failures;
    now = bd_mono_ms();
    if (connect_failures == 1 || now - last_logged_failure >= 30000) {
        hub_log("cannot connect to %s (%llu attempts so far)", gw_now, connect_failures);
        last_logged_failure = now;
    }
    if (now < discover_due) return -1;
    discover_due = now + 5000;
    if (!discover_gateway()) {
        hub_log("discovery: no gateway answered%s", "", 0);
        return -1;
    }
    hub_log("discovery: gateway answered at %s", gw_found, 0);
    if (strcmp(gw_found, gw_now) == 0) return -1; /* same address: it answers UDP but not TCP yet */
    gw_now = gw_found;
    fd = connect_gw(gw_now, gw_port);
    if (fd >= 0) {
        hub_log("connected to %s after %llu failed attempts", gw_now, connect_failures);
        connect_failures = 0;
    }
    return fd;
}
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

/* MOST fallback (upstream's tunnel, src/most): when neither the configured
   address nor discovery reaches the gateway - the gateway's Ethernet is off
   with nothing powering the OBD activation line - the DME (only) is read
   through the head unit's own UDS tunnel over MOST. Transmission reads
   (gear, turbine, wheel speeds, gearbox temperature) stay blank there. Once
   on MOST the hub stays on it; if the tunnel drops, the hub exits and the app
   starts a new one. A DSI send has no timeout, so alarm() bounds each loop. */
enum { MOST_AFTER_FAILS = 5, MOST_OPEN_MS = 2000, MOST_RETRY_MS = 30000, MOST_WAIT_MS = 2000,
       MOST_WATCHDOG_S = 60, MOST_STOP_S = 15, HSFZ_PROBE_MS = 30000 };
static int on_most, most_usable = 1, most_armed;
static uint64_t most_retry_at, hsfz_probe_at;
static const char *most_mode = "auto"; /* auto | hsfz | most */

static void hub_stop(int sig) {
    (void)sig;
    if (running && most_armed) alarm(MOST_STOP_S); /* a DSI send has no timeout */
    running = 0;
}

static void most_shutdown(void) {
    if (!most_armed) return;
    alarm(MOST_STOP_S);
    if (most_close() == 0) alarm(0); else most_usable = 0;
    on_most = 0;
    most_armed = 0;
}

static int open_most(void) {
    enum most_open_rc rc;
    most_armed = 1;
    alarm(MOST_WATCHDOG_S);
    dsi_client_set_name("diaghub");
    most_set_running(&running);
    rc = most_open(MOST_OPEN_MS);
    if (rc == MOST_UP) { hub_log("using the MOST tunnel (DME only)%s", "", 0); return 1; }
    hub_log("MOST open failed: %s", rc == MOST_NO_LIBRARY ? "no library" :
            rc == MOST_NO_TARGET ? "no target" : "no server", 0);
    if (rc == MOST_NO_LIBRARY) most_usable = 0;
    alarm(MOST_STOP_S);
    if (most_close() != 0) most_usable = 0;
    if (running) alarm(0);
    return 0;
}

/* Called after a failed connect: switch to MOST when it is time. */
static int try_most(void) {
    uint64_t now = bd_mono_ms();
    if (!most_usable || !strcmp(most_mode, "hsfz") || !running || now < most_retry_at) return 0;
    if (strcmp(most_mode, "most") && connect_failures < MOST_AFTER_FAILS) return 0;
    if (open_most()) { on_most = 1; hsfz_probe_at = bd_mono_ms() + HSFZ_PROBE_MS; return 1; }
    most_retry_at = bd_mono_ms() + MOST_RETRY_MS;
    return 0;
}

/* One request over the tunnel, in hub_read's return convention. */
static int most_exchange(unsigned ecu, const unsigned char *uds, unsigned n, const unsigned char *prefix,
                         unsigned prefix_n, unsigned char *reply, unsigned size, unsigned *negative) {
    size_t len = 0;
    enum most_rc rc;
    *negative = 0;
#ifdef MOST_ANY_ECU /* test builds only: does the tunnel reach other ECUs? */
    if (n == 0 || n > 16) { *negative = 0x11; return -2; }
#else
    if (ecu != MOST_ECU_DME || n == 0 || n > 16) { *negative = 0x11; return -2; } /* not through the tunnel */
#endif
    rc = most_request_wait(MOST_ECU_DME, uds, n, reply, size, &len, MOST_WAIT_MS);
    if (rc == MOST_CLOSED) {
        hub_log("MOST tunnel lost; exiting so the app starts a new hub%s", "", 0);
        running = 0;
        return -1;
    }
    if (rc == MOST_TIMEOUT) return -1;
    if (rc != MOST_REPLY) { *negative = 0x31; return -2; }
    if (len >= 3 && reply[0] == 0x7f) {
        *negative = reply[2];
        return -2;
    }
    if (len < prefix_n || memcmp(reply, prefix, prefix_n)) { *negative = 0x10; return -2; }
    return (int)len;
}

/* Ready to send one request now? Connects lazily and enforces the gap. */
static int may_send(void) {
    uint64_t now = bd_mono_ms(), gap;
    if (now < paused_until) return 0;
    if (lock >= 0 && someone_waiting(lock)) { pause_for(yield_ms, "lock requested"); return 0; }
    gap = min_gap_ms;
    if (avg_latency * 2 > gap) gap = avg_latency * 2;
    if (now < last_request + gap) return 0;
    /* On MOST, look for the gateway every 30 s (it can appear a minute after a
       cold start) and go back to the full-speed link with the transmission. */
    if (on_most && strcmp(most_mode, "most") && now >= hsfz_probe_at) {
        int fd = connect_gateway();
        hsfz_probe_at = bd_mono_ms() + HSFZ_PROBE_MS;
        if (fd >= 0) {
            hub_log("gateway reachable again; leaving the MOST tunnel%s", "", 0);
            most_shutdown();
            if (!most_usable) { close(fd); running = 0; return 0; } /* DSI would not stop: restart clean */
            if (lock < 0) lock = hub_reserve();
            if (lock < 0) { close(fd); paused_until = now + 500; return 0; }
            gw = fd;
            return 1;
        }
    }
    if (on_most) {
        if (lock < 0) {
            lock = hub_reserve();
            if (lock < 0) { paused_until = now + 500; return 0; }
        }
        return 1;
    }
    if (gw < 0) {
        lock = hub_reserve();
        if (lock < 0) { paused_until = now + 500; return 0; } /* a one-shot is running */
        gw = strcmp(most_mode, "most") ? connect_gateway() : -1;
        if (gw < 0 && try_most()) return 1;
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
static int hub_read_ex(unsigned ecu, const unsigned char *uds, unsigned n, const unsigned char *prefix,
                       unsigned prefix_n, unsigned char *reply, unsigned size, unsigned *negative,
                       int tolerant) {
    uint64_t start = bd_mono_ms(), took;
    int rc;
    last_request = start;
    ++requests_sent;
    rc = on_most ? most_exchange(ecu, uds, n, prefix, prefix_n, reply, size, negative)
                 : exchange(gw, ecu, uds, n, prefix, prefix_n, reply, size, request_timeout_ms, negative);
    took = bd_mono_ms() - start;
    last_request = bd_mono_ms();
    avg_latency = (avg_latency * 7 + took) >> 3;
    if (trace_hub)
        fprintf(stderr, "hub: req ecu=%02X sid=%02X len=%u rc=%d nrc=%02X took=%llu\n",
                ecu, uds[0], n, rc, rc == -2 ? *negative : 0, (unsigned long long)took);
    if (rc == -1) { failed(); return -1; }
    if (rc == -2 && !tolerant && (*negative == 0x21 || *negative == 0x22 || *negative == 0x7f)) {
        failed(); /* busy / wrong conditions / wrong session: someone else is working */
        return -2;
    }
    backoff_ms = 0;
    return rc;
}

static int hub_read(unsigned ecu, const unsigned char *uds, unsigned n, const unsigned char *prefix,
                    unsigned prefix_n, unsigned char *reply, unsigned size, unsigned *negative) {
    return hub_read_ex(ecu, uds, n, prefix, prefix_n, reply, size, negative, 0);
}

/* --- signals */
static const unsigned fast_dids[] = {0x4205, 0x5889, 0x4a49, 0x56d7, 0x586f};
static const int fast_slot[] = {BD_BOOST, BD_AFR, BD_TIMING, BD_RAIL, BD_OIL};
static const unsigned slow_dids[] = {0x4201, 0x580f, 0x4300};
static const int slow_slot[] = {-1, BD_IAT, BD_COOLANT}; /* 4201 = ambient, used for boost */

static const unsigned deck_slow_dids[] = {0x4201, 0x580f, 0x4300, 0x4408}; /* + oil temp */
static const int deck_slow_slot[] = {-1, BD_IAT, BD_COOLANT, -1};
static const unsigned temps_dids[] = {0x4408, 0x4300, 0x580f};
static const int temps_slot[] = {-1, BD_COOLANT, BD_IAT}; /* 4408 = oil, kept outside the BoostDeck frame */

static int multi_ok = 1; /* several DIDs per request until the DME refuses */
static bd_frame frame;
static uint64_t observed[BD_COUNT];
static double ambient;
static uint64_t ambient_time;
static int have_ambient;
static double oil_c;
static uint64_t oil_time;

static unsigned did_size(unsigned did) { return did == 0x580f ? 1u : 2u; }

static void store(unsigned did, int slot, const unsigned char *p, unsigned n) {
    double v;
    if (slot >= 0) frame.valid &= ~(1u << slot);
    if (!bd_decode_did(did, p, n, &v)) return;
    if (did == 0x4201) { ambient = v; ambient_time = bd_mono_ms(); have_ambient = 1; return; }
    if (did == 0x4408) { oil_c = v; oil_time = bd_mono_ms(); return; }
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

/* Engine rpm history, so converter slip can compare engine and turbine speed
   at the same instant: during a pull rpm climbs ~2000 rpm/s, and the two reads
   are up to ~200 ms apart (that alone showed ~400 rpm of false slip). */
enum { RPM_HISTORY = 6 };
static double rpm_hist_v[RPM_HISTORY];
static uint64_t rpm_hist_t[RPM_HISTORY];
static unsigned rpm_hist_n;

static void rpm_sample(int rpm, uint64_t sent, uint64_t answered) {
    unsigned i = rpm_hist_n++ % RPM_HISTORY;
    rpm_hist_v[i] = rpm;
    rpm_hist_t[i] = (sent + answered) / 2;
}

/* Engine rpm at time t: interpolated between the samples around it, or
   extrapolated from the last two by at most 120 ms. -1 if unknown. */
static double rpm_at(uint64_t t) {
    unsigned k, have = rpm_hist_n < RPM_HISTORY ? rpm_hist_n : RPM_HISTORY;
    double v0, v1;
    uint64_t t0, t1;
    if (!have) return -1;
    for (k = 1; k < have; ++k) { /* newest pair first */
        unsigned a = (rpm_hist_n - 1 - k) % RPM_HISTORY, b = (rpm_hist_n - k) % RPM_HISTORY;
        t0 = rpm_hist_t[a]; t1 = rpm_hist_t[b]; v0 = rpm_hist_v[a]; v1 = rpm_hist_v[b];
        if (t1 <= t0 || t1 - t0 > 400) break;           /* gap in the history */
        if (t >= t0 && t <= t1) return v0 + (v1 - v0) * (double)(t - t0) / (double)(t1 - t0);
        if (k == 1 && t > t1) {
            if (t - t1 > 120) return -1;
            return v1 + (v1 - v0) * (double)(t - t1) / (double)(t1 - t0);
        }
    }
    k = (rpm_hist_n - 1) % RPM_HISTORY;                 /* single fresh sample */
    t1 = rpm_hist_t[k];
    return (t >= t1 ? t - t1 : t1 - t) <= 60 ? rpm_hist_v[k] : -1;
}

static int read_shift(void) {
    unsigned char reply[16];
    unsigned negative;
    int n;
    uint64_t sent = bd_mono_ms();
    drive.rpm = drive.kph = -1;
    if (multi_pid_ok) {
        static const unsigned char req[] = {0x01, 0x0c, 0x0d}, prefix[] = {0x41};
        n = hub_read(0x12, req, 3, prefix, 1, reply, sizeof reply, &negative);
        if (n == -1) return 0;
        if (n == 6 && reply[1] == 0x0c && reply[4] == 0x0d) {
            drive.rpm = (int)(((unsigned)reply[2] * 256 + reply[3]) >> 2);
            drive.kph = reply[5];
            rpm_sample(drive.rpm, sent, bd_mono_ms());
        } else if (!(n == -2 && paused_until > bd_mono_ms())) {
            multi_pid_ok = 0;
            if (trace_hub) fprintf(stderr, "hub: combined OBD read not accepted; separate reads\n");
        }
    } else {
        static const unsigned char rq[] = {0x01, 0x0c}, rp[] = {0x41, 0x0c};
        static const unsigned char kq[] = {0x01, 0x0d}, kp[] = {0x41, 0x0d};
        n = hub_read(0x12, rq, 2, rp, 2, reply, sizeof reply, &negative);
        if (n == -1) return 0;
        if (n == 4) {
            drive.rpm = (int)(((unsigned)reply[2] * 256 + reply[3]) >> 2);
            rpm_sample(drive.rpm, sent, bd_mono_ms());
        }
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

/* --- rebuilt BoostDeck: torque, power, throttle (OBD), ethanol (MHD) */
static int deck_obd_ok = 1, ethanol_on = 0; /* ethanol_on: 0 off, 1 obd, 2 mhd, 3 auto */
static int ethanol_obd_ok = 1, ethanol_source = ETHANOL_NONE;
static double ref_torque, deck_torque, deck_rpm, deck_throttle, ethanol_pct;
static uint64_t deck_time, ethanol_time;

static unsigned pid_size(unsigned pid) { return pid == 0x0c || pid == 0x63 ? 2u : 1u; }

static int read_deck(void) {
    /* 01 0C 11 62 (63): rpm, throttle, actual torque %, reference torque until known. */
    unsigned char req[] = {0x01, 0x0c, 0x11, 0x62, 0x63}, prefix[] = {0x41}, reply[32];
    unsigned negative, at = 1, n_req = ref_torque > 0 ? 4u : 5u;
    int n = hub_read(0x12, req, n_req, prefix, 1, reply, sizeof reply, &negative), got = 0;
    double torque = -1, rpm = -1, throttle = -1;
    if (n == -1) return 0;
    if (n == -2) {
        if (!(paused_until > bd_mono_ms())) deck_obd_ok = 0; /* refused: stop asking */
        return 1;
    }
    while (n > 0 && at < (unsigned)n) {
        unsigned pid = reply[at], size = pid_size(pid), a;
        if (at + 1 + size > (unsigned)n) break;
        a = reply[at + 1];
        if (pid == 0x0c) { rpm = (a * 256 + reply[at + 2]) / 4.0; ++got; }
        else if (pid == 0x11) { throttle = a * 100 / 255.0; ++got; }
        else if (pid == 0x62) { torque = (double)a - 125; ++got; }
        else if (pid == 0x63) { unsigned r = a * 256 + reply[at + 2]; if (r >= 100 && r <= 2000) ref_torque = r; }
        at += 1 + size;
    }
    if (got == 3 && ref_torque > 0) {
        deck_torque = torque / 100 * ref_torque;
        deck_rpm = rpm;
        deck_throttle = throttle;
        deck_time = bd_mono_ms();
    }
    return 1;
}

/* Standard OBD PID 52, ethanol fuel %: A x 100 / 255. */
static int read_ethanol_obd(void) {
    static const unsigned char req[] = {0x01, 0x52}, prefix[] = {0x41, 0x52};
    unsigned char reply[8];
    unsigned negative;
    int n = hub_read_ex(0x12, req, 2, prefix, 2, reply, sizeof reply, &negative, 1);
    if (n == -1) return 0;
    if (n == 3) {
        ethanol_pct = reply[2] * 100 / 255.0;
        ethanol_time = bd_mono_ms();
        ethanol_source = ETHANOL_OBD;
        return 1;
    }
    return -1;
}

/* MHD+ monitor block (as upstream BoostDeck reads it): the tune's active
   ethanol %. Only when asked for (--ethanol mhd/auto). Any refusal just waits a minute. */
static int read_ethanol_mhd(void) {
    static const unsigned char query[] = {0x2c, 0x88, 0x01}, prefix[] = {0x6c, 0x88};
    unsigned char reply[38];
    unsigned negative;
    int n = hub_read_ex(0x12, query, sizeof query, prefix, sizeof prefix, reply, sizeof reply, &negative, 1);
    if (n == -1) return 0;
    if (n == 38 && reply[17] <= 100) {
        ethanol_pct = reply[17];
        ethanol_time = bd_mono_ms();
        ethanol_source = ETHANOL_MHD;
        return 1;
    }
    return -1; /* not available: caller backs off */
}

/* One ethanol reading from the chosen source; -1 = not available (back off). */
static int read_ethanol(void) {
    int r;
    if (ethanol_on == 2) return read_ethanol_mhd();
    if (ethanol_on == 1 || ethanol_obd_ok) {
        r = read_ethanol_obd();
        if (r < 0 && ethanol_on == 3) {
            ethanol_obd_ok = 0; /* auto: the DME doesn't do PID 52, use MHD from now on */
            if (trace_hub) fprintf(stderr, "hub: OBD ethanol (01 52) not answered; MHD block\n");
            return 1;
        }
        return r;
    }
    return read_ethanol_mhd();
}

/* --- EGS (0x18): gear, turbine speed, wheel speeds */
static int egs_multi_ok = 1, wheels_ok = 1;
static int32_t gear_now;
/* Turbine speed history: each sample is paired with engine rpm at the same
   moment once an rpm reading from after that moment is in. OBD rpm runs about
   90 ms behind the engine (fitted on a drive log: in-gear |rpm - turbine| is
   smallest at 80-100 ms), so the pair uses rpm reported 90 ms later. */
enum { TURBINE_HISTORY = 6, RPM_LAG_MS = 90 };
static double turbine_v[TURBINE_HISTORY];
static uint64_t turbine_t[TURBINE_HISTORY];
static unsigned turbine_n;
static uint64_t gear_time;
static double wheel_front, wheel_driven;
static uint64_t wheel_time;

static void store_egs(unsigned did, const unsigned char *p, uint64_t sent) {
    unsigned w = p[0] * 256u + p[1];
    uint64_t now = bd_mono_ms();
    if (did == 0xda2e) { gear_now = w >= 1 && w <= 8 ? (int32_t)w : w == 0x0a ? -1 : 0; gear_time = now; }
    else if (w < 16000) { /* DA34 */
        unsigned i = turbine_n++ % TURBINE_HISTORY;
        turbine_v[i] = w;
        turbine_t[i] = (sent + now) / 2;
    }
}

static int read_gear(void) {
    unsigned char reply[32], req[5] = {0x22, 0xda, 0x2e, 0xda, 0x34}, prefix[3] = {0x62, 0xda, 0x2e};
    unsigned negative;
    int n;
    uint64_t sent = bd_mono_ms();
    if (egs_multi_ok) {
        n = hub_read(0x18, req, 5, prefix, 3, reply, sizeof reply, &negative);
        if (n == -1) return 0;
        if (n == 9 && reply[5] == 0xda && reply[6] == 0x34) {
            store_egs(0xda2e, reply + 3, sent);
            store_egs(0xda34, reply + 7, sent);
            return 1;
        }
        if (!(n == -2 && negative == 0x21)) {
            egs_multi_ok = 0;
            if (trace_hub) fprintf(stderr, "hub: EGS multi-DID read not accepted; single reads\n");
        }
        return 1;
    }
    n = hub_read(0x18, req, 3, prefix, 3, reply, sizeof reply, &negative);
    if (n == -1) return 0;
    if (n == 5) store_egs(0xda2e, reply + 3, sent);
    while (running && !may_send()) {
        if (bd_mono_ms() < paused_until) return 1;
        nap(5);
    }
    req[2] = 0x34; prefix[2] = 0x34;
    sent = bd_mono_ms();
    n = hub_read(0x18, req, 3, prefix, 3, reply, sizeof reply, &negative);
    if (n == -1) return 0;
    if (n == 5) store_egs(0xda34, reply + 3, sent);
    return 1;
}

static int read_wheels(void) {
    static const unsigned char req[] = {0x22, 0xda, 0x1d}, prefix[] = {0x62, 0xda, 0x1d};
    unsigned char reply[16];
    unsigned negative, i;
    double w[4];
    int n = hub_read(0x18, req, 3, prefix, 3, reply, sizeof reply, &negative);
    if (n == -1) return 0;
    if (n != 11) {
        if (n == -2 && negative != 0x21) wheels_ok = 0; /* not on this car: OBD speed only */
        return 1;
    }
    for (i = 0; i < 4; ++i) w[i] = ((int)(reply[3 + 2 * i] * 256u + reply[4 + 2 * i]) - 0x7fff) * WHEEL_KPH;
    hub_wheels(w, &wheel_front, &wheel_driven);
    if (wheel_front < .3) wheel_front = 0; /* standing still */
    wheel_time = bd_mono_ms();
    return 1;
}

/* --- FuelBlend: tank level */
static double fuel_pct;
static uint64_t fuel_time;
static int read_fuel(void) {
    static const unsigned char req[] = {0x01, 0x2f}, prefix[] = {0x41, 0x2f};
    unsigned char reply[8];
    unsigned negative;
    int n = hub_read(0x12, req, 2, prefix, 2, reply, sizeof reply, &negative);
    if (n == -1) return 0;
    if (n == 3) { fuel_pct = reply[2] * 100 / 255.0; fuel_time = bd_mono_ms(); }
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

static int32_t tenths(double v, uint64_t seen, uint64_t now, uint64_t stale) {
    if (!seen || now - seen > stale) return TEMPS_NONE;
    return (int32_t)(v * 10 + (v < 0 ? -.5 : .5));
}

static void send_temps(int sock, struct sockaddr_in *dest, int paused) {
    struct temps_frame t;
    uint64_t now = bd_mono_ms();
    t.sampled_ms = now;
    t.oil = t.coolant = t.intake = t.trans = TEMPS_NONE;
    if (!paused) {
        t.oil = tenths(oil_c, oil_time, now, TEMPS_STALE_MS);
        t.coolant = tenths(frame.value[BD_COOLANT], observed[BD_COOLANT], now, TEMPS_STALE_MS);
        t.intake = tenths(frame.value[BD_IAT], observed[BD_IAT], now, TEMPS_STALE_MS);
        t.trans = tenths(frame.value[BD_TRANS], observed[BD_TRANS], now, trans_stale_ms);
    }
    sendto(sock, &t, sizeof t, 0, (struct sockaddr *)dest, sizeof *dest);
}

static int32_t fixed(double v, double scale, uint64_t seen, uint64_t now, uint64_t stale) {
    double x;
    if (!seen || now - seen > stale) return HUB_NONE;
    x = v * scale;
    return (int32_t)(x + (x < 0 ? -.5 : .5));
}

static void send_deck(int sock, struct sockaddr_in *dest, int paused) {
    struct deck_frame d;
    uint64_t now = bd_mono_ms();
    d.sampled_ms = now;
    d.oil_temp = d.torque = d.power = d.throttle = d.ethanol = HUB_NONE;
    if (!paused) {
        d.oil_temp = fixed(oil_c, 10, oil_time, now, TEMPS_STALE_MS);
        d.torque = fixed(deck_torque, 10, deck_time, now, DECK_STALE_MS);
        d.power = fixed(deck_torque * deck_rpm / 9549.3, 10, deck_time, now, DECK_STALE_MS);
        d.throttle = fixed(deck_throttle, 10, deck_time, now, DECK_STALE_MS);
        d.ethanol = fixed(ethanol_pct, 10, ethanol_time, now, 15000);
    }
    sendto(sock, &d, sizeof d, 0, (struct sockaddr *)dest, sizeof *dest);
}

static void send_gear(int sock, struct sockaddr_in *dest, int paused) {
    struct gear_frame g;
    uint64_t now = bd_mono_ms();
    int fresh = !paused && gear_time && now - gear_time < GEAR_STALE_MS;
    double rpm = -1;
    unsigned k, have = turbine_n < TURBINE_HISTORY ? turbine_n : TURBINE_HISTORY;
    uint64_t rpm_newest = rpm_hist_n ? rpm_hist_t[(rpm_hist_n - 1) % RPM_HISTORY] : 0;
    g.sampled_ms = now;
    g.rpm = g.turbine = HUB_NONE;
    g.sample = 0;
    /* Newest turbine sample whose matching rpm (RPM_LAG_MS later) has been read. */
    for (k = 0; fresh && k < have; ++k) {
        unsigned id = turbine_n - 1 - k, i = id % TURBINE_HISTORY;
        if (now - turbine_t[i] > 600) break;
        if (turbine_t[i] + RPM_LAG_MS > rpm_newest) continue;
        rpm = rpm_at(turbine_t[i] + RPM_LAG_MS);
        if (rpm >= 0) {
            g.rpm = (int32_t)(rpm + 0.5);
            g.turbine = (int32_t)turbine_v[i];
            g.sample = (int32_t)(id + 1);
        }
        break;
    }
    g.gear = fresh ? gear_now : 0;
    sendto(sock, &g, sizeof g, 0, (struct sockaddr *)dest, sizeof *dest);
}

static void send_fuel(int sock, struct sockaddr_in *dest, int paused) {
    struct fuel_frame f;
    uint64_t now = bd_mono_ms();
    memset(&f, 0, sizeof f);
    f.sampled_ms = now;
    f.level = paused ? HUB_NONE : fixed(fuel_pct, 10, fuel_time, now, FUEL_STALE_MS);
    f.ethanol = paused || !ethanol_on ? HUB_NONE : fixed(ethanol_pct, 10, ethanol_time, now, ETHANOL_STALE_MS);
    f.source = f.ethanol == HUB_NONE ? ETHANOL_NONE : ethanol_source;
    sendto(sock, &f, sizeof f, 0, (struct sockaddr *)dest, sizeof *dest);
}

static void send_timer(int sock, struct sockaddr_in *dest) {
    struct timer_frame t;
    uint64_t now = bd_mono_ms();
    int fresh = wheel_time && now - wheel_time < TIMER_FRAME_STALE_MS;
    t.sampled_ms = fresh ? wheel_time : drive.sampled_ms;
    t.kph = drive.sampled_ms && now - drive.sampled_ms < 1000 ? drive.kph : -1;
    t.front = fresh ? (int32_t)(wheel_front * 100 + .5) : HUB_NONE;
    t.driven = fresh ? (int32_t)(wheel_driven * 100 + .5) : HUB_NONE;
    sendto(sock, &t, sizeof t, 0, (struct sockaddr *)dest, sizeof *dest);
}

int main(int argc, char **argv) {
    unsigned boost_port = BD_UDP_PORT, drive_port = DRIVE_PORT, control_port = hub_port, timer_out = timer_port,
             temps_out = TEMPS_PORT, deck_out = DECK_PORT, gear_out = GEAR_PORT,
             fuel_out = FUEL_PORT;
    unsigned cycles = 0, done = 0, i;
    int me = C_SHIFT, control = -1, out, explicit_client = 0;
    pid_t parent = getppid();
    struct sockaddr_in boost_dest, drive_dest, timer_dest, temps_dest, deck_dest, gear_dest, fuel_dest, control_dest;
    uint64_t seen[C_LAST + 1] = {0}, next_beat = 0, due_fast = 0, due_slow = 0, due_trans = 0,
             due_shift = 0, due_guard = 0, due_temps = 0, last_boost_frame = 0, last_temps_frame = 0,
             due_deck = 0, due_gear = 0, due_wheels = 0, due_ethanol = 0, last_deck_frame = 0,
             last_gear_frame = 0, due_fuel = 0, last_fuel_frame = 0;

    for (i = 1; i < (unsigned)argc; ++i) {
        if (!strcmp(argv[i], "--host") && i + 1 < (unsigned)argc && !gw_host) gw_host = argv[++i];
        else if (!strcmp(argv[i], "--session")) { if (!explicit_client) me = C_BOOST; }
        else if (!strcmp(argv[i], "--client") && i + 1 < (unsigned)argc) {
            ++i; explicit_client = 1;
            if (!strcmp(argv[i], "boost")) me = C_BOOST;
            else if (!strcmp(argv[i], "shift")) me = C_SHIFT;
            else if (!strcmp(argv[i], "timer")) me = C_TIMER;
            else if (!strcmp(argv[i], "temps")) me = C_TEMPS;
            else if (!strcmp(argv[i], "deck")) me = C_DECK;
            else if (!strcmp(argv[i], "gear")) me = C_GEAR;
            else if (!strcmp(argv[i], "fuel")) me = C_FUEL;
            else return 2;
        }
        else if (!strcmp(argv[i], "--port") && i + 1 < (unsigned)argc) gw_port = number(argv[++i], 1, 65535);
        else if (!strcmp(argv[i], "--discover") && i + 2 < (unsigned)argc) { /* tests: ADDR PORT, any responder */
            discover_addr = argv[++i];
            discover_port = number(argv[++i], 1, 65535);
            discover_any = 1;
        }
        else if (!strcmp(argv[i], "--log") && i + 1 < (unsigned)argc) log_path = argv[++i];
        else if (!strcmp(argv[i], "--transport") && i + 1 < (unsigned)argc) {
            most_mode = argv[++i];
            if (strcmp(most_mode, "auto") && strcmp(most_mode, "hsfz") && strcmp(most_mode, "most")) return 2;
        }
        else if (!strcmp(argv[i], "--udp-port") && i + 1 < (unsigned)argc) {
            unsigned p = number(argv[++i], 1, 65535);
            if (me == C_BOOST) boost_port = p; else if (me == C_TIMER) timer_out = p;
            else if (me == C_TEMPS) temps_out = p; else drive_port = p;
        }
        else if (!strcmp(argv[i], "--boost-port") && i + 1 < (unsigned)argc) boost_port = number(argv[++i], 1, 65535);
        else if (!strcmp(argv[i], "--drive-port") && i + 1 < (unsigned)argc) drive_port = number(argv[++i], 1, 65535);
        else if (!strcmp(argv[i], "--timer-port") && i + 1 < (unsigned)argc) timer_out = number(argv[++i], 1, 65535);
        else if (!strcmp(argv[i], "--temps-port") && i + 1 < (unsigned)argc) temps_out = number(argv[++i], 1, 65535);
        else if (!strcmp(argv[i], "--deck-port") && i + 1 < (unsigned)argc) deck_out = number(argv[++i], 1, 65535);
        else if (!strcmp(argv[i], "--gear-port") && i + 1 < (unsigned)argc) gear_out = number(argv[++i], 1, 65535);
        else if (!strcmp(argv[i], "--fuel-port") && i + 1 < (unsigned)argc) fuel_out = number(argv[++i], 1, 65535);
        else if (!strcmp(argv[i], "--ethanol")) {
            /* obd | mhd | auto; bare --ethanol (older BoostDeck plugin) = mhd */
            const char *m = i + 1 < (unsigned)argc ? argv[i + 1] : "";
            if (!strcmp(m, "obd")) { ethanol_on = 1; ++i; }
            else if (!strcmp(m, "auto")) { ethanol_on = 3; ++i; }
            else { ethanol_on = 2; if (!strcmp(m, "mhd")) ++i; }
        }
        else if (!strcmp(argv[i], "--hub-port") && i + 1 < (unsigned)argc) control_port = number(argv[++i], 1, 65535);
        else if (!strcmp(argv[i], "--lock-port") && i + 1 < (unsigned)argc) lock_port = number(argv[++i], 1, 65535);
        else if (!strcmp(argv[i], "--guard-pause-ms") && i + 1 < (unsigned)argc) guard_pause_ms = number(argv[++i], 100, 600000);
        else if (!strcmp(argv[i], "--cycles") && i + 1 < (unsigned)argc) cycles = number(argv[++i], 1, 1000000);
        else if (!strcmp(argv[i], "--trace")) trace_hub = 1;
        else if (!strcmp(argv[i], "--no-trans")) {}
        else {
            fprintf(stderr, "Usage: %s --host IPv4 [--session | --client boost|shift|timer|temps|deck|gear|fuel] [--ethanol obd|mhd|auto] [--port 6801] [--trace]\n", argv[0]);
            return 2;
        }
    }
    if (!gw_host || parent <= 1) return 2;
    {
        struct sigaction sa;
        sigset_t alarm_set;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = hub_stop;
        sigaction(SIGINT, &sa, 0); sigaction(SIGTERM, &sa, 0);
        signal(SIGPIPE, SIG_IGN); signal(SIGALRM, SIG_DFL); /* the MOST watchdog kills a stuck hub */
        sigemptyset(&alarm_set); sigaddset(&alarm_set, SIGALRM);
        sigprocmask(SIG_UNBLOCK, &alarm_set, 0);
    }
    out = udp_socket(0, 0);
    if (out < 0) return 1;
    to_loopback(&boost_dest, boost_port);
    to_loopback(&drive_dest, drive_port);
    to_loopback(&timer_dest, timer_out);
    to_loopback(&temps_dest, temps_out);
    to_loopback(&deck_dest, deck_out);
    to_loopback(&gear_dest, gear_out);
    to_loopback(&fuel_dest, fuel_out);
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
                if (b[0] == C_BOOST || b[0] == C_SHIFT || b[0] == C_TIMER || b[0] == C_TEMPS ||
                    b[0] == C_DECK || b[0] == C_GEAR || b[0] == C_FUEL) seen[(int)b[0]] = now;
        }
        wanted = me;
        if (seen[C_BOOST] && now - seen[C_BOOST] < subscriber_ttl_ms) wanted |= C_BOOST;
        if (seen[C_SHIFT] && now - seen[C_SHIFT] < subscriber_ttl_ms) wanted |= C_SHIFT;
        if (seen[C_TIMER] && now - seen[C_TIMER] < subscriber_ttl_ms) wanted |= C_TIMER;
        if (seen[C_TEMPS] && now - seen[C_TEMPS] < subscriber_ttl_ms) wanted |= C_TEMPS;
        if (seen[C_DECK] && now - seen[C_DECK] < subscriber_ttl_ms) wanted |= C_DECK;
        if (seen[C_GEAR] && now - seen[C_GEAR] < subscriber_ttl_ms) wanted |= C_GEAR;
        if (seen[C_FUEL] && now - seen[C_FUEL] < subscriber_ttl_ms) wanted |= C_FUEL;
        if (wanted & C_DECK) wanted |= C_BOOST; /* the rebuilt BoostDeck also takes NBG3 frames */
        if (wanted & C_GEAR) wanted |= C_SHIFT;

        if (now >= paused_until && gw < 0 && lock < 0 && trace_hub && paused_until)
            fprintf(stderr, "hub: resuming\n");
        if (may_send()) {
            /* Earliest-due task first (the most overdue read goes next), list order on ties.
               A fixed priority starved the gear read on the car: the DME's ~40 ms answers
               stretch the request gap to the rpm period, so rpm was due on every turn. */
            enum { T_NONE, T_GUARD, T_WHEELS, T_SHIFT, T_GEAR, T_FUEL, T_SLOW, T_TEMPS, T_TRANS,
                   T_DECK, T_ETHANOL, T_FAST };
            int task = T_NONE;
            uint64_t best = 0;
#define READY(t, cond, due) \
            if ((cond) && now >= (due) && (task == T_NONE || (due) < best)) { task = (t); best = (due); }
            READY(T_GUARD, guard_supported, due_guard)
            READY(T_WHEELS, (wanted & C_TIMER) && wheels_ok, due_wheels)
            READY(T_SHIFT, wanted & (C_SHIFT | C_TIMER), due_shift)
            READY(T_GEAR, wanted & C_GEAR, due_gear)
            READY(T_FUEL, wanted & C_FUEL, due_fuel)
            READY(T_SLOW, wanted & C_BOOST, due_slow)
            READY(T_TEMPS, wanted & C_TEMPS, due_temps)
            READY(T_TRANS, wanted & (C_BOOST | C_TEMPS), due_trans)
            READY(T_DECK, (wanted & C_DECK) && deck_obd_ok, due_deck)
            READY(T_ETHANOL, (wanted & (C_DECK | C_FUEL)) && ethanol_on, due_ethanol)
            READY(T_FAST, wanted & C_BOOST, due_fast)
#undef READY
            switch (task) {
            case T_GUARD:
                due_guard = now + guard_period_ms;
                sent = check_session();
                break;
            case T_WHEELS:
                due_wheels = now + wheel_ms;
                if ((sent = read_wheels()) && bd_mono_ms() >= paused_until && wheel_time)
                    send_timer(out, &timer_dest);
                break;
            case T_SHIFT:
                due_shift = now + ((wanted & C_SHIFT) ? shift_ms : wheels_ok ? timer_obd_ms : timer_ms);
                if ((sent = read_shift()) && now >= paused_until) {
                    if (wanted & C_SHIFT)
                        sendto(out, &drive, sizeof drive, 0, (struct sockaddr *)&drive_dest, sizeof drive_dest);
                    if ((wanted & C_TIMER) && !wheels_ok) send_timer(out, &timer_dest);
                }
                break;
            case T_GEAR:
                due_gear = now + gear_ms;
                sent = read_gear();
                break;
            case T_FUEL:
                due_fuel = now + fuel_ms;
                sent = read_fuel();
                break;
            case T_SLOW:
                due_slow = now + boost_slow_ms;
                if (wanted & C_DECK) sent = read_group(deck_slow_dids, deck_slow_slot, 4);
                else sent = read_group(slow_dids, slow_slot, 3);
                break;
            case T_TEMPS:
                due_temps = now + temps_ms;
                sent = read_group(temps_dids, temps_slot, 3);
                break;
            case T_TRANS:
                due_trans = now + trans_ms;
                sent = read_trans();
                break;
            case T_DECK:
                due_deck = now + deck_ms;
                sent = read_deck();
                break;
            case T_ETHANOL: {
                int r = read_ethanol();
                due_ethanol = now + (r < 0 ? ethanol_retry_ms : ethanol_ms);
                sent = r != 0;
                break;
            }
            case T_FAST:
                due_fast = now + boost_fast_ms;
                sent = read_group(fast_dids, fast_slot, 5);
                if (cycles && ++done >= cycles) running = 0;
                break;
            }
            (void)sent;
        }
        if ((wanted & C_BOOST) && now - last_boost_frame >= boost_fast_ms) {
            if (now < paused_until) frame.valid = 0; /* paused: show gauges as unavailable */
            send_boost(out, &boost_dest);
            last_boost_frame = now;
        }
        if ((wanted & C_DECK) && now - last_deck_frame >= deck_ms) {
            send_deck(out, &deck_dest, now < paused_until);
            last_deck_frame = now;
        }
        if ((wanted & C_GEAR) && now - last_gear_frame >= gear_ms / 2) {
            send_gear(out, &gear_dest, now < paused_until);
            last_gear_frame = now;
        }
        if ((wanted & C_FUEL) && now - last_fuel_frame >= 1000) {
            send_fuel(out, &fuel_dest, now < paused_until);
            last_fuel_frame = now;
        }
        if ((wanted & C_TEMPS) && now - last_temps_frame >= temps_ms) {
            send_temps(out, &temps_dest, now < paused_until);
            last_temps_frame = now;
        }
        if (on_most) alarm(running ? MOST_WATCHDOG_S : MOST_STOP_S);
        nap(gw >= 0 ? 5 : 20);
    }
    disconnect();
    most_shutdown();
    if (control >= 0) close(control);
    close(out);
    if (trace_hub) fprintf(stderr, "hub: exit after %u requests\n", requests_sent);
    return 0;
}
