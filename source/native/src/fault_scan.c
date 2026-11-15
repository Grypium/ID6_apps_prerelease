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
    max_lines = 120
};

static int scan_faults(int fd) {
    unsigned char request[] = {0,0,0,5,0,1,0xf4,0xdf,0x19,0x02,scan_status};
    static unsigned char body[4096], seen[256];
    static char lines[max_lines][16];
    unsigned char header[6];
    unsigned modules = 0, faults = 0, shown = 0, pending = 0, i;
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
            unsigned char alive[8] = {0,0,0,2,0,0x13,0,0xf4};
            if (!transfer(fd, alive, 8, 1, deadline)) return -1;
            continue;
        }
        if (ctrl != 1 || len < 3 || body[1] != 0xf4) continue;
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
        for (i = 5; i + 4 <= len; i += 4) {
            unsigned status = body[i+3] & body[4]; /* only bits the module supports */
            if (!(status & fault_bits)) continue;
            ++faults;
            if (shown < max_lines) {
                snprintf(lines[shown], sizeof lines[shown], "%02X %02X%02X%02X %02X",
                         body[0], body[i], body[i+1], body[i+2], body[i+3]);
                ++shown;
            }
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
    lock = reserve_gateway();
    if (lock < 0) {
        puts(errno == EADDRINUSE ? "Diagnostics busy. Wait a moment and try again."
                                 : "Could not reserve the diagnostics connection");
        return 0;
    }
    gw = connect_gw(host, port);
    if (gw < 0) puts("Diagnostic gateway unavailable");
    else if (scan_faults(gw) < 0) puts("Scan interrupted. Try again.");
    if (gw >= 0) close(gw);
    close(lock);
    return 0;
}
