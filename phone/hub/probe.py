#!/usr/bin/env python3
"""Read-only check of what the hub relies on, through the phone's relay (127.0.0.1:6801).
Sends only 22 (read data) and 01 (OBD current data) requests, as tester 0xF5."""
import socket, struct, time
def ask(target, uds, wait=2.0):
    s = socket.create_connection(("127.0.0.1", 6801), timeout=5)
    body = bytes([0xF5, target]) + uds
    s.sendall(struct.pack(">IH", len(body), 1) + body)
    end = time.time() + wait; buf = b""; s.settimeout(0.3)
    try:
        while time.time() < end:
            try: buf += s.recv(4096)
            except socket.timeout: pass
            while len(buf) >= 6:
                n, ctrl = struct.unpack(">IH", buf[:6])
                if len(buf) < 6 + n: break
                b, buf = buf[6:6+n], buf[6+n:]
                if ctrl == 0x12: s.sendall(struct.pack(">IH", 2, 0x13) + b"\x00\xf5"); continue
                if ctrl == 1 and b[0] == target and b[1] == 0xF5:
                    if b[2] == 0x7F and len(b) > 4 and b[4] == 0x78: continue
                    return b[2:]
    finally: s.close()
    return None
checks = [("session (22 F186)", 0x12, "22f186"),
          ("fast boost group, one request", 0x12, "2242055889" "4a4956d7586f"),
          ("slow group, one request", 0x12, "22420158" "0f4300"),
          ("rpm+speed, one request", 0x12, "010c0d"),
          ("transmission temp (EGS)", 0x18, "22da12")]
for name, t, hexreq in checks:
    r = ask(t, bytes.fromhex(hexreq))
    print(f"{name:32s} -> {r.hex() if r else 'no answer'}")
