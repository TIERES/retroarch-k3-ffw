#!/bin/bash
# Builds the two-PC rollback harness (see README.md). Run from an MSYS2 MINGW64 shell, after the
# fork itself was configured/built (it reuses the exact compile flags of kaillera_sync.c).
#   kaillera/tests/rollback_harness/build.sh [output dir, default /tmp/ksync_harness]
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
OUT=${1:-/tmp/ksync_harness}
mkdir -p "$OUT"
cd "$ROOT"
FLAGS=$(make OUTDIR=out -n -W kaillera/kaillera_sync.c out/obj-unix/release/kaillera/kaillera_sync.o \
   | grep "gcc.*kaillera/kaillera_sync.c" | head -1 | sed 's|^[^ ]*gcc||; s| -MMD .*$||; s|-Dmain=SDL_main||')
[ -n "$FLAGS" ] || { echo "could not get the compile flags of kaillera_sync.c (make -n)"; exit 1; }
gcc $FLAGS -c -o "$OUT/kaillera_sync.o" kaillera/kaillera_sync.c
gcc $FLAGS -c -o "$OUT/stubs.o" "$HERE/stubs.c"
gcc -shared -o "$OUT/ksync_h.dll" "$OUT/kaillera_sync.o" "$OUT/stubs.o" -Wl,--export-all-symbols -luser32
gcc -O2 -o "$OUT/driver.exe" "$HERE/driver.c"
for m in A B; do
   mkdir -p "$OUT/$m/logs"
   cp "$OUT/ksync_h.dll" "$OUT/$m/"
   head -c 100000 /dev/zero | tr '\0' 'x' > "$OUT/$m/game.bin"
done
echo "OK: cd $OUT && ./driver.exe 7200 1500,3000,4500,6000"
