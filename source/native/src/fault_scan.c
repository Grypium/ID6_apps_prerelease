/* fault_scan.c - included by collector.c when built with -DFAULT_SCAN_ONLY.
 *
 * Read-only engine (DME) fault scan, the same view a tuning tool gives:
 *   - fault memory:  ReadDTCInformation 19 02 0C (pending + confirmed codes)
 *   - shadow memory: ReadDataByIdentifier 22 2000 (BMW dtcShadowMemory),
 *     records of 3-byte code + status, de-duplicated
 * Only the DME (0x12) is asked, with physical requests. Nothing is written
 * or cleared.
 *
 * Output (stdout, read by the FaultCodes plugin):
 *   line 1: a summary sentence
 *   then "12 CCCCCC SS" per fault-memory code and "SH CCCCCC SS" per shadow
 *   code (code, raw status byte).
 */
enum {
    dme = 0x12,
    fault_mask = 0x0c, /* pending + confirmed, as BMW's own fault reads use */
    max_lines = 120,
    /* Our own tester address. The gauge workers use 0xF4 and the gateway
     * delivers replies by tester address, so sharing 0xF4 would mix their
     * replies with ours. 0xF5 is accepted and keeps the scan separate. */
    tester = 0xf5
};

static unsigned shown;
static char lines[max_lines][16];

/* One physical request to `ecu`; returns the positive reply length (starting
 * at the response SID) or -1. Waits through "response pending" (7F xx 78). */
static int ask(int fd, unsigned ecu, const unsigned char *uds, unsigned uds_len,
               unsigned char *body, unsigned size) {
    unsigned char request[16], header[6];
    uint64_t deadline = bd_mono_ms() + 2500;
    unsigned i;
    if (uds_len > sizeof request - 8) return -1;
    request[0] = request[1] = 0;
    request[2] = (unsigned char)((uds_len + 2) >> 8);
    request[3] = (unsigned char)(uds_len + 2);
    request[4] = 0; request[5] = 1;
    request[6] = tester; request[7] = (unsigned char)ecu;
    for (i = 0; i < uds_len; ++i) request[8 + i] = uds[i];
    if (!transfer(fd, request, 8 + uds_len, 1, deadline)) return -1;
    while (running) {
        uint32_t len, k;
        unsigned ctrl;
        if (!transfer(fd, header, 6, 0, deadline)) return -1;
        len = ((uint32_t)header[0]<<24)|((uint32_t)header[1]<<16)|((uint32_t)header[2]<<8)|header[3];
        ctrl = ((unsigned)header[4]<<8)|header[5];
        if (len > size || !transfer(fd, body, len, 0, deadline)) return -1;
        if (ctrl == 0x12) { /* gateway alive check */
            unsigned char alive[8] = {0,0,0,2,0,0x13,0,tester};
            if (!transfer(fd, alive, 8, 1, deadline)) return -1;
            continue;
        }
        if (ctrl != 1 || len < 3 || body[0] != ecu || body[1] != tester) continue;
        if (body[2] == 0x7f) {
            if (len >= 5 && body[3] == uds[0] && body[4] == 0x78) {
                deadline = bd_mono_ms() + 2500;
                continue;
            }
            if (len >= 5 && body[3] == uds[0]) return -1;
            continue;
        }
        if (body[2] != (unsigned char)(uds[0] + 0x40)) continue;
        for (k = 2; k < len; ++k) body[k-2] = body[k];
        return (int)(len - 2);
    }
    return -1;
}

/* The DME is often busy serving the gauge apps; give it a few tries. */
static int ask_retry(int fd, const unsigned char *uds, unsigned uds_len,
                     unsigned char *body, unsigned size) {
    int n = -1, tries;
    for (tries = 0; tries < 3 && running; ++tries) {
        n = ask(fd, dme, uds, uds_len, body, size);
        if (n >= 0) break;
        poll(0, 0, 300);
    }
    return n;
}

static void add_line(const char *tag, const unsigned char *r) {
    if (shown >= max_lines) return;
    snprintf(lines[shown], sizeof lines[shown], "%s %02X%02X%02X %02X", tag, r[0], r[1], r[2], r[3]);
    ++shown;
}

static int scan_faults(int fd) {
    static const unsigned char read_faults[] = {0x19, 0x02, fault_mask};
    static const unsigned char read_shadow[] = {0x22, 0x20, 0x00};
    static unsigned char body[4096];
    unsigned faults = 0, shadows = 0, i, j;
    int n;

    /* 59 02 <availability> then 4-byte records. */
    n = ask_retry(fd, read_faults, sizeof read_faults, body, sizeof body);
    if (n < 3 || body[1] != 0x02) { puts("The engine module (DME) did not answer. Is the ignition on?"); return 0; }
    for (i = 3; i + 4 <= (unsigned)n; i += 4)
        if (body[i+3] & fault_mask) { add_line("12", body + i); ++faults; }

    /* 62 20 00 then 4-byte records; the same code can appear more than once. */
    n = ask_retry(fd, read_shadow, sizeof read_shadow, body, sizeof body);
    if (n >= 3 && body[1] == 0x20 && body[2] == 0x00) {
        for (i = 3; i + 4 <= (unsigned)n; i += 4) {
            int repeat = 0;
            if (!body[i] && !body[i+1] && !body[i+2]) continue;
            for (j = 3; j < i && !repeat; j += 4)
                repeat = body[j] == body[i] && body[j+1] == body[i+1] && body[j+2] == body[i+2];
            if (!repeat) { add_line("SH", body + i); ++shadows; }
        }
        printf("Engine (DME): %u fault %s, %u shadow %s\n", faults, faults == 1 ? "code" : "codes",
               shadows, shadows == 1 ? "code" : "codes");
    } else {
        printf("Engine (DME): %u fault %s (shadow memory unavailable)\n", faults, faults == 1 ? "code" : "codes");
    }
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
    /* Newer gauge workers hand the shared lock over when asked (each failed
     * attempt asks). Older BoostDeck builds hold it for good, so after a short
     * wait carry on without it: this scan is read-only and uses its own
     * gateway connection and tester address. */
    {
        uint64_t give_up = bd_mono_ms() + 1500;
        while ((lock = reserve_gateway()) < 0 && errno == EADDRINUSE && running &&
               bd_mono_ms() < give_up)
            poll(0, 0, 20);
    }
    gw = connect_gw(host, port);
    if (gw < 0) puts("Diagnostic gateway unavailable");
    else if (scan_faults(gw) < 0) puts("Scan interrupted. Try again.");
    if (gw >= 0) close(gw);
    if (lock >= 0) close(lock);
    return 0;
}
