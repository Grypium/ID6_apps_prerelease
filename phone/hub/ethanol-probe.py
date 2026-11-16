#!/usr/bin/env python3
"""Find the ethanol reading (parked, engine running, relay on 127.0.0.1:6801).
Read-only by default: OBD PID support bitmap (01 40) and PID 52 (ethanol %).
--mhd also sends the MHD monitor request (2C 88 01) once - not a standard read;
only after checking with the owner. Tester 0xF5 (don't open FaultCodes meanwhile)."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hsfz import GW
g = GW()
def ask(uds):
    r = g.ask(0x12, bytes.fromhex(uds), 1.0)
    return r[0].hex() if r and r[0] != "ctrl" else repr(r)
bitmap = ask("0140")
print("01 40 ->", bitmap)
if bitmap.startswith("4140") and len(bitmap) >= 12:
    bits = int(bitmap[4:12], 16)
    print("PID 52 supported:", bool(bits >> (32 - (0x52 - 0x40)) & 1))
r = ask("0152")
print("01 52 ->", r, f"= E{int(r[4:6], 16) * 100 / 255:.1f}" if r.startswith("4152") and len(r) >= 6 else "")
if "--mhd" in sys.argv:
    r = ask("2c8801")
    print("2C 88 01 ->", r)
    if r.startswith("6c88") and len(r) >= 36: print("MHD active ethanol byte 17:", int(r[34:36], 16))
