#!/bin/sh
#
# Phase 3: zcc running as a Zeitlos app.
#
# Builds a filesystem image, runs sw/apps/zcc/zcc.bin under
# sim/zsim-headless against it, and checks three things:
#
#   1. the device compiler produces an image at all;
#   2. that image RUNS, under the same simulator;
#   3. it is byte-for-byte identical to what the host zcc produces
#      from the same source.
#
# (3) is the one that matters. The device and host builds share every
# line of the compiler and differ only in port_dev.c vs port_host.c
# (see zcc_port.h), so an identical output means the seam is the only
# difference -- which is exactly the claim being made.
#
# This is as far as it is possible to check without hardware. What it
# does NOT cover: the real SD card, the real scheduler, and anything
# involving `wm`.

set -e

here=$(cd "$(dirname "$0")" && pwd)
top="$here/../../../.."
zcc="$here/../zcc"
devzcc="$here/../zcc.bin"
sim="$top/sim/zsim-headless"
work="${TMPDIR:-/tmp}/zcc-dev"

# zcc.bin is an ordinary Zeitlos app (newlib, sw/common/riscv-app.ld),
# so building it needs a toolchain with a C library. A machine without
# one can still run the HOST suite (tests/run.sh), which is where the
# compiler's behaviour is actually checked -- this suite adds the
# device port and the ZEXE image, nothing about code generation.
[ -f "$devzcc" ] || {
    echo "no zcc.bin -- run 'make' in sw/apps/zcc (needs a newlib toolchain)" >&2
    exit 1
}
[ -x "$sim" ] || { echo "no simulator -- run 'make' in sim/" >&2; exit 1; }

rm -rf "$work"
mkdir -p "$work/libz" "$work/include" "$work/common"

cp "$here/../libz/libz.bin" "$here/../libz/libz.sym" "$here/../libz/libz.h" \
   "$work/libz/"
cp "$here"/../include/*.h "$work/include/"
cp "$top"/sw/common/*.h "$top"/sw/common/syscalls.def "$work/common/"

pass=0
fail=0

for src in "$here"/t0*.c "$here"/l0*.c; do
    name=$(basename "$src" .c)
    cp "$src" "$work/$name.c"
    cp "$here"/*.h "$work/" 2>/dev/null || true

    echo "-I /include -I /libz -I /common -L /libz -o /$name.bin /$name.c" \
        > "$work/zcc.args"

    if ! "$sim" -q -r "$work" -n 4000000000 -m 4194304 "$devzcc" \
            > "$work/$name.devlog" 2>&1; then
        echo "FAIL $name: the device compiler did not finish"
        sed 's/^/      /' "$work/$name.devlog" | head -5
        fail=$((fail + 1))
        continue
    fi

    if [ ! -f "$work/$name.bin" ]; then
        echo "FAIL $name: no output produced"
        sed 's/^/      /' "$work/$name.devlog" | head -5
        fail=$((fail + 1))
        continue
    fi

    "$zcc" -I "$here" -I "$here/../include" -I "$here/../libz" \
        -I "$top/sw/common" -L "$here/../libz" \
        -o "$work/$name.host.bin" "$src" 2>/dev/null

    if ! cmp -s "$work/$name.bin" "$work/$name.host.bin"; then
        echo "FAIL $name: device and host output differ"
        fail=$((fail + 1))
        continue
    fi

    # And it has to run.
    if ! "$sim" -q -r "$work" -n 400000000 "$work/$name.bin" \
            > "$work/$name.out" 2>&1; then
        echo "FAIL $name: the device-compiled binary did not run"
        fail=$((fail + 1))
        continue
    fi

    echo "ok   $name"
    pass=$((pass + 1))
done

echo
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
