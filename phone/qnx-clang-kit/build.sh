#!/data/data/com.termux/files/usr/bin/bash
# Build the NBT Evo apps for QNX 6.5 armle-v7 with Termux clang + lld,
# without the QNX SDK. Usage: build.sh NATIVE_SRC_DIR OUT_DIR
set -euo pipefail
KIT="$(cd "$(dirname "$0")" && pwd)"
SRC="$(cd "$1" && pwd)"
OUT="$2"
mkdir -p "$OUT/obj"

COMMON=(--target=armv7a-linux-gnueabi -march=armv7-a -marm -mfpu=vfpv3-d16 -mfloat-abi=softfp
  -mno-unaligned-access -nostdlibinc -isystem "$KIT/include"
  -U__linux__ -U__linux -Ulinux -U__gnu_linux__ -U__unix__ -U__unix -Uunix
  -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-common
  -O2 -Wall -Wextra -Wno-unused-command-line-argument)
CC() { clang "${COMMON[@]}" -std=c99 "$@"; }
CXX() { clang "${COMMON[@]}" -x c++ -std=gnu++98 -fclang-abi-compat=17 -mno-constructor-aliases -fno-exceptions -fno-threadsafe-statics -fno-strict-aliasing "$@"; }

LDCOMMON=(--hash-style=sysv -z norelro -z nognustack --no-rosegment --build-id=none
  -z max-page-size=4096 -L "$KIT/stubs")
EXE=(-dynamic-linker /usr/lib/ldqnx.so.2 --image-base=0x100000)

clang --target=armv7a-linux-gnueabi -c "$KIT/crt/crt1.S" -o "$OUT/obj/crt1.o"
clang --target=armv7a-linux-gnueabi -c "$KIT/crt/note.S" -o "$OUT/obj/note.o"
clang --target=armv7a-linux-gnueabi -c "$KIT/crt/uldivmod.S" -o "$OUT/obj/uldivmod.o"

O="$OUT/obj"
CC -fPIC -c "$KIT/crt/helpers.c" -o "$O/helpers.o"
# Feature flags mirror upstream native/build.sh (2026-11-15); older sources ignore them.
CC -DPERF_LAB -I"$SRC/src" -fPIC -c "$SRC/src/gauge_core.c" -o "$O/core.o"

# Plugins (shared libraries loaded by the iDrive HMI)
plugin() { # name source "defines" extra-objs...
  local name=$1 src=$2 defs=$3; shift 3
  CXX $defs -I"$SRC/src" -I"$SRC/sdk" -fPIC -c "$src" -o "$O/$name.o"
  ld.lld -shared -soname "lib$name.so" "${LDCOMMON[@]}" --no-undefined \
    "$O/$name.o" "$@" "$O/helpers.o" "$O/note.o" \
    -l:libAppFramework.A.so -l:libCoreFramework.A.so "${LIBS[@]}" -o "$OUT/lib$name.so"
}
LIBS=(-l:libm.so.2 -l:libsocket.so.3 -l:libstdc++.so.6 -l:libc.so.3)
plugin BoostDeck "$SRC/src/plugin.cpp" "-DPERF_LAB" "$O/core.o"
plugin ShiftDeck "$SRC/src/shift_deck.cpp" "-DPERF_LAB -DNATIVE_SHIFT_SMOOTH" "$O/core.o"
LIBS=(-l:libstdc++.so.6 -l:libm.so.2 -l:libc.so.3)
plugin MhdMaps "$SRC/src/mhd_maps.cpp" "-DPERF_LAB"
if [ -f "$SRC/src/fault_codes.cpp" ]; then
  plugin FaultCodes "$SRC/src/fault_codes.cpp" ""
fi

# Worker executables
worker() { # outname "collector defines" "core defines"
  local name=$1 defs=$2 coredefs=$3
  CC -fno-pic -I"$SRC/src" $defs -c "$SRC/src/collector.c" -o "$O/$name-collector.o"
  CC -fno-pic -I"$SRC/src" $coredefs -c "$SRC/src/gauge_core.c" -o "$O/$name-core.o"
  ld.lld "${LDCOMMON[@]}" "${EXE[@]}" "$O/crt1.o" "$O/$name-collector.o" "$O/$name-core.o" "$O/helpers.o" "$O/uldivmod.o" "$O/note.o" \
    -l:libm.so.2 -l:libsocket.so.3 -l:libc.so.3 -o "$OUT/$name"
}
worker boostdeck-collector-qnx "-DNATIVE_FAST -DPERF_LAB" "-DPERF_LAB"
worker mhd-maps-qnx "-DMHD_MAPS_ONLY" ""
worker shift-deck-qnx "-DNATIVE_FAST -DSHIFT_DECK_ONLY" "-DPERF_LAB"
[ -f "$SRC/src/fault_scan.c" ] && worker fault-codes-qnx "-DFAULT_SCAN_ONLY" ""
# Shared read-only diagnostic hub; installed as both boostdeck-collector-qnx and
# shift-deck-qnx for the official plugins, so gauge_core is built without
# PERF_LAB (NBG3 frames, which the official BoostDeck plugin parses).
[ -f "$SRC/src/diag_hub.c" ] && worker diag-hub-qnx "-DDIAG_HUB" ""

# File-operations helper (official build is static; this one links libc dynamically)
CC -fno-pic -c "$SRC/fileops.c" -o "$O/fileops.o"
ld.lld "${LDCOMMON[@]}" "${EXE[@]}" "$O/crt1.o" "$O/fileops.o" "$O/helpers.o" "$O/note.o" -l:libc.so.3 -o "$OUT/gaugedeck-fileops"

ls -l "$OUT" | grep -v obj
