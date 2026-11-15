/* fault_scan.c - included by collector.c when built with -DFAULT_SCAN_ONLY.
 *
 * Read-only fault memory scan of every module. One functional (broadcast)
 * UDS ReadDTCInformation request (19 02) goes to address 0xDF through the
 * diagnostic gateway; each module answers from its own address. Nothing is
 * written or cleared.
 *
 * Output (stdout, read by the FaultCodes plugin):
 *   line 1: a summary sentence
 *   then one "EE CCCCCC SS" line per fault: module address, 3-byte code,
 *   raw status byte.
 */
enum {
    scan_status = 0xaf, /* failed, this cycle, pending, confirmed, since clear, lamp */
    fault_bits = 0x2f,  /* counts only with a real failure bit; lamp bit alone is not a fault */
    max_modules = 64,
    max_lines = 120,
    /* Our own tester address. The gauge workers use 0xF4 and the gateway
     * delivers replies by tester address, so sharing 0xF4 would mix their
     * replies with ours. 0xF5 is accepted and keeps the scan separate. */
    tester = 0xf5
};

static unsigned faults, shown;
static char lines[max_lines][16];

/* reply = 59 02 <availability> then 4-byte records (3-byte code + status). */
static void add_codes(unsigned ecu, const unsigned char *reply, unsigned n) {
    unsigned i;
    for (i = 3; i + 4 <= n; i += 4) {
        unsigned status = reply[i+3] & reply[2]; /* only bits the module supports */
        if (!(status & fault_bits)) continue;
        ++faults;
        if (shown < max_lines) {
            snprintf(lines[shown], sizeof lines[shown], "%02X %02X%02X%02X %02X",
                     ecu, reply[i], reply[i+1], reply[i+2], reply[i+3]);
            ++shown;
        }
    }
}

/* Modules worth a direct (physical) request if they missed the broadcast:
 * engine and transmission, which the gauge apps keep busy. */
static const unsigned char must_ask[] = {0x12, 0x18};

/* Physical 19 02 request to one module; returns the reply length (59 02 ...) or -1. */
static int ask_module(int fd, unsigned ecu, unsigned char *body, unsigned size) {
    unsigned char request[] = {0,0,0,5,0,1,tester,0,0x19,0x02,scan_status}, header[6];
    uint64_t deadline = bd_mono_ms() + 2500;
    request[7] = (unsigned char)ecu;
    if (!transfer(fd, request, sizeof request, 1, deadline)) return -1;
    while (running) {
        uint32_t len;
        unsigned ctrl;
        if (!transfer(fd, header, 6, 0, deadline)) return -1;
        len = ((uint32_t)header[0]<<24)|((uint32_t)header[1]<<16)|((uint32_t)header[2]<<8)|header[3];
        ctrl = ((unsigned)header[4]<<8)|header[5];
        if (len > size || !transfer(fd, body, len, 0, deadline)) return -1;
        if (ctrl == 0x12) {
            unsigned char alive[8] = {0,0,0,2,0,0x13,0,tester};
            if (!transfer(fd, alive, 8, 1, deadline)) return -1;
            continue;
        }
        if (ctrl != 1 || len < 5 || body[0] != ecu || body[1] != tester) continue;
        if (body[2] == 0x7f) {
            if (body[4] == 0x78) { deadline = bd_mono_ms() + 2500; continue; }
            return -1;
        }
        if (body[2] != 0x59 || body[3] != 0x02) continue;
        { uint32_t k; for (k = 2; k < len; ++k) body[k-2] = body[k]; }
        return (int)(len - 2);
    }
    return -1;
}

static int scan_faults(int fd) {
    unsigned char request[] = {0,0,0,5,0,1,tester,0xdf,0x19,0x02,scan_status};
    static unsigned char body[4096], seen[256];
    unsigned char header[6];
    unsigned modules = 0, pending = 0, i;
    uint64_t deadline, quiet;

    if (!transfer(fd, request, sizeof request, 1, bd_mono_ms() + 1000)) return -1;
    deadline = bd_mono_ms() + 12000; /* hard cap for the whole scan */
    quiet = bd_mono_ms() + 6000;     /* finish once nothing new arrives for a while */
    while (running) {
        uint64_t now = bd_mono_ms(), until = quiet < deadline ? quiet : deadline;
        uint32_t len;
        unsigned ctrl;
        if (now >= deadline || (now >= until && !pending)) break;
        if (!transfer(fd, header, 6, 0, pending ? deadline : until)) break;
        len = ((uint32_t)header[0]<<24)|((uint32_t)header[1]<<16)|((uint32_t)header[2]<<8)|header[3];
        ctrl = ((unsigned)header[4]<<8)|header[5];
        if (len > sizeof body || !transfer(fd, body, len, 0, deadline)) return -1;
        if (ctrl == 0x12) { /* gateway alive check */
            unsigned char alive[8] = {0,0,0,2,0,0x13,0,tester};
            if (!transfer(fd, alive, 8, 1, deadline)) return -1;
            continue;
        }
        if (ctrl != 1 || len < 3 || body[1] != tester) continue;
        if (len == 5 && body[2] == 0x7f && body[3] == 0x19) {
            /* 0x78 = response pending: that module will answer later */
            if (body[4] == 0x78 && !(seen[body[0]] & 3)) { seen[body[0]] |= 2; ++pending; }
            quiet = bd_mono_ms() + 4000;
            continue;
        }
        if (len < 5 || body[2] != 0x59 || body[3] != 0x02 || (seen[body[0]] & 1)) continue;
        if (seen[body[0]] & 2) { seen[body[0]] &= (unsigned char)~2u; --pending; }
        seen[body[0]] |= 1;
        if (++modules > max_modules) break;
        quiet = bd_mono_ms() + 4000;
        add_codes(body[0], body + 2, len - 2);
    }
    /* Follow up directly with modules that missed the broadcast: those that
     * said "response pending" but never answered, and engine/transmission. */
    for (i = 0; i < 256 && running; ++i) {
        unsigned k, ask = (seen[i] & 3) == 2;
        int n;
        for (k = 0; k < sizeof must_ask; ++k)
            if (must_ask[k] == i && !(seen[i] & 1)) ask = 1;
        if (!ask) continue;
        /* The engine module is often busy serving the gauge apps; give it a
         * few tries before giving up. */
        for (k = 0; k < 3 && running; ++k) {
            n = ask_module(fd, i, body, sizeof body);
            if (n >= 3) break;
            poll(0, 0, 300);
        }
        if (n >= 3) {
            seen[i] |= 1;
            ++modules;
            add_codes(i, body, (unsigned)n);
        }
    }
    if (!modules) { puts("No modules answered. Is the ignition on?"); return 0; }
    if (!faults) printf("No fault codes (%u modules checked)\n", modules);
    else printf("%u fault %s in %u modules\n", faults, faults == 1 ? "code" : "codes", modules);
    for (i = 0; i < shown; ++i) puts(lines[i]);
    return 0;
}

int main(int argc, char **argv) {
    const char *host = 0;
    unsigned port = 6801;
    int lock, gw, i;
    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--host") && i+1 < argc && !host) host = argv[++i];
        else if (!strcmp(argv[i], "--port") && i+1 < argc) port = number(argv[++i], 1, 65535);
        else break;
    }
    if (i != argc || !host) {
        fprintf(stderr, "Usage: %s --host IPv4 [--port 6801]\n", argv[0]);
        return 2;
    }
    signal(SIGINT, stop); signal(SIGTERM, stop); signal(SIGPIPE, SIG_IGN);
    /* The gauge workers hold this lock in short leases and hand it over when
     * asked (each failed attempt below asks), retaking it ~165 ms later.
     * Retry quickly for a few seconds so we catch the hand-over. */
    {
        uint64_t give_up = bd_mono_ms() + 1500;
        while ((lock = reserve_gateway()) < 0 && errno == EADDRINUSE && running &&
               bd_mono_ms() < give_up)
            poll(0, 0, 20);
    }
    /* Older BoostDeck builds hold the lock for good and never hand it over.
     * This scan is read-only and uses its own gateway connection (the gateway
     * accepts several), so carry on without the lock in that case. */
    gw = connect_gw(host, port);
    if (gw < 0) puts("Diagnostic gateway unavailable");
    else if (scan_faults(gw) < 0) puts("Scan interrupted. Try again.");
    if (gw >= 0) close(gw);
    if (lock >= 0) close(lock);
    return 0;
}
