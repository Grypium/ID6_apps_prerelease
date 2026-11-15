#!/data/data/com.termux/files/usr/bin/bash
# Generate link-only stub libraries (symbol names only, no code) for the QNX 6.5
# head unit libraries. The real libraries on the car are used at run time.
set -euo pipefail
cd "$(dirname "$0")"
for s in *.S; do
  lib="${s%.S}"
  clang --target=armv7a-linux-gnueabi -c "$s" -o "$lib.o"
  extra=()
  [ "$lib" = libsocket.so.3 ] && extra=(--version-script libsocket.ver)  # symbols are versioned libsocket.so.2
  ld.lld -shared -soname "$lib" --hash-style=sysv "${extra[@]}" "$lib.o" -o "$lib"
  rm -f "$lib.o"
done
ls -1 *.so*
