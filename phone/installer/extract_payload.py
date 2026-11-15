#!/usr/bin/env python3
"""Extract the official app payload (payload/...) from the Windows release of the
NBT Evo Gauge Installer (a Go program with an embed.FS), with no Windows needed.

    python3 extract_payload.py "NBT Evo Gauge Installer.exe" OUT_DIR

The release zip is in this repo's history:
    git show f4ec31e:NBT-Evo-Gauges-Windows-x64.zip > win.zip && unzip win.zip

How it works: Go stores each embedded file as {name string, data string, hash
[16]byte} (48 bytes on amd64), where a string is {pointer, length}. We find the
name of one known file, find the record that points at it, then walk the record
array in both directions.
"""
import os, struct, sys

ANCHOR = b"payload/BoostDeck/BoostDeck.manifest"
RECORD = 48


def sections(pe):
    off = struct.unpack_from("<I", pe, 0x3C)[0]
    if pe[off:off + 4] != b"PE\0\0":
        sys.exit("not a PE file")
    nsec = struct.unpack_from("<H", pe, off + 6)[0]
    optsize = struct.unpack_from("<H", pe, off + 20)[0]
    opt = off + 24
    if struct.unpack_from("<H", pe, opt)[0] != 0x20B:
        sys.exit("expected a 64-bit (PE32+) executable")
    base = struct.unpack_from("<Q", pe, opt + 24)[0]
    out = []
    for i in range(nsec):
        s = opt + optsize + 40 * i
        vsize, va, rsize, raw = struct.unpack_from("<IIII", pe, s + 8)
        out.append((base + va, max(vsize, rsize), raw, rsize))
    return out


def main():
    exe, dest = sys.argv[1], sys.argv[2]
    pe = open(exe, "rb").read()
    secs = sections(pe)

    def to_va(off):
        for va, size, raw, rsize in secs:
            if raw <= off < raw + rsize:
                return va + off - raw
        return None

    def to_off(va):
        for sva, size, raw, rsize in secs:
            if sva <= va < sva + rsize:
                return raw + va - sva
        return None

    def string(ptr, n):
        o = to_off(ptr)
        if o is None or n < 0 or o + n > len(pe):
            return None
        return pe[o:o + n]

    def record(at):
        if at < 0 or at + RECORD > len(pe):
            return None
        nptr, nlen, dptr, dlen = struct.unpack_from("<QQQQ", pe, at)
        if not 0 < nlen < 512:
            return None
        name = string(nptr, nlen)
        if not name or not name.startswith(b"payload"):
            return None
        data = string(dptr, dlen) if dlen else b""
        if data is None:
            return None
        return name.decode(), data

    # Find the anchor name's address, then the record that points at it.
    found = None
    start = 0
    while found is None:
        o = pe.find(ANCHOR, start)
        if o < 0:
            sys.exit("could not find the embedded payload")
        va = to_va(o)
        key = struct.pack("<QQ", va, len(ANCHOR))
        r = pe.find(key)
        if r >= 0:
            found = r
        start = o + 1

    first = found
    while record(first - RECORD):
        first -= RECORD
    files, at = [], first
    while (rec := record(at)):
        files.append(rec)
        at += RECORD
    written = 0
    for name, data in files:
        if name.endswith("/"):
            continue
        rel = os.path.normpath(name)
        if rel.startswith("..") or os.path.isabs(rel):
            sys.exit(f"unsafe path {name!r}")
        path = os.path.join(dest, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)
        written += 1
        print(f"{len(data):9d}  {rel}")
    print(f"extracted {written} files to {dest}")


if __name__ == "__main__":
    main()
