#!/bin/sh
#
# zfpga on the device, under sim/.
#
#   ./tests/run_dev.sh
#
# Builds zfpga.bin (needs RISCV_PREFIX, see docs/toolchain.md) and
# sim/zsim-headless, lays out a card directory the way a release does --
# /fpga/lfe5u25f.zdb -- and runs zfpga.bin against it. Each output must
# be byte-identical to the HOST build's output for the same command, and
# so, through tests/run.sh, to ecppack's.
#
# Instruction counts are printed because they are the cost on hardware:
# at roughly 12 MIPS, 12M instructions is a second. What this cannot
# show is the SD card, which the simulator backs with a host directory;
# reading the 1.3MB database off a real card is unmeasured
# (docs/zfpga.md, "What it costs to run").

cd "$(dirname "$0")/.." || exit 1
TREE=$(cd ../../.. && pwd)

make -s -f Makefile.host || exit 1
make -s || exit 1
make -s -C "$TREE/sim" zsim-headless || exit 1

OUT=tests/out/dev
CARD=$OUT/card
rm -rf "$OUT"
mkdir -p "$CARD/fpga"
cp db/lfe5u25f.zdb "$CARD/fpga/"

for f in blink dfu edge; do
    gunzip -c tests/fixtures/$f.config.gz > "$CARD/$f.config"
done
printf '.device LFE5U-25F\n' > "$CARD/empty.config"

pass=0; fail=0

# name | arguments, card-relative paths
while IFS='|' read -r name args; do
    name=$(echo "$name" | sed 's/ *$//')
    args=$(echo "$args" | sed 's/^ *//')
    [ -z "$name" ] && continue

    # The device reads its command line from /zfpga.args (the launch
    # argument's fallback); the host gets the same words as argv.
    echo "$args -o /$name.bit" > "$CARD/zfpga.args"
    if ! "$TREE/sim/zsim-headless" -r "$CARD" -m 33554432 -n 0 zfpga.bin \
            > "$OUT/$name.out" 2> "$OUT/$name.err"; then
        echo "FAIL $name: simulator exit $?"; cat "$OUT/$name.out"; fail=$((fail + 1)); continue
    fi
    hostargs=$(echo "$args" | sed "s| /| $CARD/|g")
    # shellcheck disable=SC2086
    ./zfpga $hostargs -D db -o "$OUT/$name.host.bit" > /dev/null || { echo "FAIL $name: host"; fail=$((fail + 1)); continue; }

    insns=$(sed -n 's/.*: \([0-9]*\) instructions/\1/p' "$OUT/$name.err")
    if cmp -s "$CARD/$name.bit" "$OUT/$name.host.bit"; then
        echo "ok   $name  $(wc -c < "$CARD/$name.bit") bytes, $insns instructions"
        pass=$((pass + 1))
    else
        echo "FAIL $name: device and host differ"; cat "$OUT/$name.out"
        fail=$((fail + 1))
    fi
done << 'EOF'
empty      | pack /empty.config
blink      | pack /blink.config
blink_c    | pack /blink.config -c -f 2.4
blink_boot | pack /blink.config -c -a 0x190000
dfu_boot   | pack /dfu.config -c -a 0x040000
edge       | pack /edge.config
EOF

# build on the device, from the card layout a release installs (db/ is
# /fpga): the bitstream must be the host's, and the peak memory must
# leave room in the 4MB the kernel gives zfpga -- stack and heap share
# it -- so the limit here is 3.5MB.
mkdir -p "$CARD/fpga" "$OUT/hostbuild"
cp -r db/boards db/examples "$CARD/fpga/"
for f in blink.v on.zn hand.zl; do
    echo "build /fpga/examples/$f -b lakritz -D /fpga" > "$CARD/zfpga.args"
    if ! "$TREE/sim/zsim-headless" -r "$CARD" -m 33554432 -n 0 zfpga.bin \
            > "$OUT/build.out" 2> "$OUT/build.err"; then
        echo "FAIL build $f: simulator"; cat "$OUT/build.out"; fail=$((fail + 1)); continue
    fi
    cp "db/examples/$f" "$OUT/hostbuild/"
    ./zfpga build "$OUT/hostbuild/$f" -b lakritz -D db > /dev/null
    peak=$(sed -n 's/.*peak memory \([0-9]*\) KB.*/\1/p' "$OUT/build.out" | head -1)
    insns=$(sed -n 's/.*: \([0-9]*\) instructions/\1/p' "$OUT/build.err")
    if ! cmp -s "$CARD/fpga/examples/${f%.*}.bit" "$OUT/hostbuild/${f%.*}.bit"; then
        echo "FAIL build $f: device and host bitstreams differ"; fail=$((fail + 1))
    elif [ -z "$peak" ] || [ "$peak" -gt 3584 ]; then
        echo "FAIL build $f: peak memory ${peak:-?} KB, over the 3.5MB budget"; fail=$((fail + 1))
    else
        echo "ok   build $f  $insns instructions, peak $peak KB"; pass=$((pass + 1))
    fi
done

# 8.3 on the device: an input whose outputs would not fit the card is
# refused before any work, by name (docs/zfpga.md sec. 22).
cp db/examples/on.zn "$CARD/fpga/examples/longnames.zn"
echo "build /fpga/examples/longnames.zn -b lakritz -D /fpga" > "$CARD/zfpga.args"
"$TREE/sim/zsim-headless" -r "$CARD" -m 33554432 -n 0 zfpga.bin > "$OUT/long.out" 2>&1
if grep -q "is not an 8.3 name" "$OUT/long.out" && [ ! -e "$CARD/fpga/examples/longnames.bit" ]; then
    echo "ok   build longnames.zn: refused, not an 8.3 name"; pass=$((pass + 1))
else
    echo "FAIL build longnames.zn: not refused"; cat "$OUT/long.out"; fail=$((fail + 1))
fi

# zfpga jump on the device: the host's jumploader, byte for byte
mkdir -p "$CARD/fpga/boards"
cp db/boards/lakritz.brd db/boards/lakritz.lpf "$CARD/fpga/boards/"
echo "jump 0x190000 -b lakritz -D /fpga -o /jump.bit" > "$CARD/zfpga.args"
"$TREE/sim/zsim-headless" -r "$CARD" -m 33554432 -n 0 zfpga.bin > "$OUT/jump.out" 2> "$OUT/jump.err"
./zfpga jump 0x190000 -b lakritz -D db -o "$OUT/jump.host.bit" > /dev/null 2>&1
insns=$(sed -n 's/.*: \([0-9]*\) instructions/\1/p' "$OUT/jump.err")
if cmp -s "$CARD/jump.bit" "$OUT/jump.host.bit" && [ ! -e "$CARD/jump.v" ]; then
    echo "ok   jump 0x190000 -b lakritz  $insns instructions"; pass=$((pass + 1))
else
    echo "FAIL jump on the device: not the host's jumploader"; cat "$OUT/jump.out"; fail=$((fail + 1))
fi

# a 45F build on the device (Mozart ML1): the host's bitstream, in a peak
# that leaves room in zfpga's 4 MB tier. The device's peak, not the
# host's: 64-bit pointers make the host's larger (3.8 MB against 3.6).
cp db/lfe5u45f.zdb "$CARD/fpga/"
cp db/boards/mozart1.brd db/boards/mozart1.lpf "$CARD/fpga/boards/"
echo "build /fpga/examples/blink.v -b mozart_ml1 -o /b45.bit" > "$CARD/zfpga.args"
"$TREE/sim/zsim-headless" -r "$CARD" -m 33554432 -n 0 zfpga.bin > "$OUT/b45.out" 2> "$OUT/b45.err"
cp examples/blink.v "$OUT/b45.v"       # a copy: build writes beside its input
./zfpga build "$OUT/b45.v" -b mozart_ml1 -D db -o "$OUT/b45.host.bit" > "$OUT/b45.host.out" 2>&1
insns=$(sed -n 's/.*: \([0-9]*\) instructions/\1/p' "$OUT/b45.err")
dpeak=$(grep -o "peak memory [0-9]* KB" "$OUT/b45.out" | grep -o "[0-9]*")
hpeak=$(grep -o "peak memory [0-9]* KB" "$OUT/b45.host.out" | grep -o "[0-9]*")
if cmp -s "$CARD/b45.bit" "$OUT/b45.host.bit" && [ -n "$dpeak" ] && [ "$dpeak" -lt 4096 ]; then
    echo "ok   build blink.v -b mozart_ml1 (45F)  $insns instructions, peak $dpeak KB of 4096"; pass=$((pass + 1))
else
    echo "FAIL a 45F build on the device (peak ${dpeak:-?} KB; the host's ${hpeak:-?} KB):"; tail -3 "$OUT/b45.out"; fail=$((fail + 1))
fi

# zfpga flash on a machine with no writable flash (the simulator has no
# flash controller): a clear refusal, not a crash or a hang
cp examples/blink.v "$OUT/bl.v"
./zfpga build "$OUT/bl.v" -b lakritz -D db -o "$CARD/blink.bit" > /dev/null 2>&1
echo "flash /blink.bit" > "$CARD/zfpga.args"
"$TREE/sim/zsim-headless" -r "$CARD" -m 33554432 -n 0 zfpga.bin > "$OUT/flash.out" 2> "$OUT/flash.err"
if grep -q "no writable flash here" "$OUT/flash.out" "$OUT/flash.err"; then
    echo "ok   flash, with no writable flash: refused, clearly"; pass=$((pass + 1))
else
    echo "FAIL flash on the simulator:"; cat "$OUT/flash.out"; fail=$((fail + 1))
fi

# synth on the device, of a hierarchy with an `include: the host's .zl
cp examples/hier.v examples/hierdefs.vh examples/hier.lpf "$CARD/"
echo "synth /hier.v -l /hier.lpf -o /hier.zl" > "$CARD/zfpga.args"
"$TREE/sim/zsim-headless" -r "$CARD" -m 33554432 -n 0 zfpga.bin > "$OUT/hier.out" 2> "$OUT/hier.err"
./zfpga synth examples/hier.v -l examples/hier.lpf -o "$OUT/hier.host.zl" > /dev/null 2>&1
insns=$(sed -n 's/.*: \([0-9]*\) instructions/\1/p' "$OUT/hier.err")
if [ -f "$CARD/hier.zl" ] &&
   [ "$(tail -n +2 "$CARD/hier.zl" | md5sum)" = "$(tail -n +2 "$OUT/hier.host.zl" | md5sum)" ]; then
    echo "ok   synth hier (5 instances, an \`include)  $insns instructions"; pass=$((pass + 1))
else
    echo "FAIL synth hier: device and host differ"; cat "$OUT/hier.out"; fail=$((fail + 1))
fi

# bram on the device, on a compressed multiboot .bit: the host's result
./zfpga pack tests/fixtures/bram/rom.config -D db -c -a 0x040000 -o "$CARD/rom.bit" > /dev/null 2>&1
cp tests/fixtures/bram/seed.hex tests/fixtures/bram/new.hex "$CARD/"
echo "bram /rom.bit -f /seed.hex -t /new.hex -o /romnew.bit -D /fpga" > "$CARD/zfpga.args"
"$TREE/sim/zsim-headless" -r "$CARD" -m 33554432 -n 0 zfpga.bin > "$OUT/bram.out" 2> "$OUT/bram.err"
./zfpga bram "$CARD/rom.bit" -f tests/fixtures/bram/seed.hex -t tests/fixtures/bram/new.hex \
    -o "$OUT/romnew.host.bit" -D db > /dev/null 2>&1
insns=$(sed -n 's/.*: \([0-9]*\) instructions/\1/p' "$OUT/bram.err")
if cmp -s "$CARD/romnew.bit" "$OUT/romnew.host.bit"; then
    echo "ok   bram .bit  $insns instructions"; pass=$((pass + 1))
else
    echo "FAIL bram .bit: device and host differ"; cat "$OUT/bram.out"; fail=$((fail + 1))
fi

# synth on the device: the same .zl as the host, from the Verilog
cp examples/features.v examples/features.lpf "$CARD/"
echo "synth /features.v -l /features.lpf -o /features.zl" > "$CARD/zfpga.args"
if "$TREE/sim/zsim-headless" -r "$CARD" -m 33554432 -n 0 zfpga.bin > "$OUT/synth.out" 2> "$OUT/synth.err"; then
    ./zfpga synth examples/features.v -l examples/features.lpf -o "$OUT/features.host.zl" > /dev/null
    insns=$(sed -n 's/.*: \([0-9]*\) instructions/\1/p' "$OUT/synth.err")
    if [ "$(tail -n +2 "$CARD/features.zl" | md5sum)" = "$(tail -n +2 "$OUT/features.host.zl" | md5sum)" ]; then
        echo "ok   synth features  $insns instructions"; pass=$((pass + 1))
    else
        echo "FAIL synth features: device and host differ"; cat "$OUT/synth.out"; fail=$((fail + 1))
    fi
else
    echo "FAIL synth features: simulator"; cat "$OUT/synth.out"; fail=$((fail + 1))
fi

# unpack on the device: the same text as the host, from the bitstreams
# the device itself just packed.
for name in blink blink_c dfu_boot; do
    echo "unpack /$name.bit -o /unpack_$name.config" > "$CARD/zfpga.args"
    if ! "$TREE/sim/zsim-headless" -r "$CARD" -m 33554432 -n 0 zfpga.bin \
            > "$OUT/unpack_$name.out" 2> "$OUT/unpack_$name.err"; then
        echo "FAIL unpack $name: simulator"; cat "$OUT/unpack_$name.out"; fail=$((fail + 1)); continue
    fi
    ./zfpga unpack "$CARD/$name.bit" -D db -o "$OUT/unpack_$name.host.config" > /dev/null
    insns=$(sed -n 's/.*: \([0-9]*\) instructions/\1/p' "$OUT/unpack_$name.err")
    if cmp -s "$CARD/unpack_$name.config" "$OUT/unpack_$name.host.config"; then
        echo "ok   unpack $name  $insns instructions"; pass=$((pass + 1))
    else
        echo "FAIL unpack $name: device and host differ"; cat "$OUT/unpack_$name.out"; fail=$((fail + 1))
    fi
done

# Phase 3 on the device: zfpga pnr must write the same .config as the
# host build for the same netlist.
cp examples/on.zn examples/blink.zn "$CARD/"
for name in on blink; do
    echo "pnr /$name.zn -o /$name.config" > "$CARD/zfpga.args"
    if ! "$TREE/sim/zsim-headless" -r "$CARD" -m 33554432 -n 0 zfpga.bin \
            > "$OUT/pnr_$name.out" 2> "$OUT/pnr_$name.err"; then
        echo "FAIL pnr $name: simulator"; cat "$OUT/pnr_$name.out"; fail=$((fail + 1)); continue
    fi
    ./zfpga pnr "examples/$name.zn" -D db -o "$OUT/pnr_$name.host.config" > /dev/null
    insns=$(sed -n 's/.*: \([0-9]*\) instructions/\1/p' "$OUT/pnr_$name.err")
    if cmp -s "$CARD/$name.config" "$OUT/pnr_$name.host.config"; then
        echo "ok   pnr $name  $insns instructions"; pass=$((pass + 1))
    else
        echo "FAIL pnr $name: device and host differ"; cat "$OUT/pnr_$name.out"; fail=$((fail + 1))
    fi
done

# Phase 4 on the device: the router is deterministic, so the device
# must choose exactly the host's routes.
cp tests/fixtures/blink_route.zn tests/fixtures/sides_route.zn tests/fixtures/dense_route.zn "$CARD/"
for name in blink_route sides_route dense_route; do
    echo "pnr /$name.zn -o /$name.config" > "$CARD/zfpga.args"
    if ! "$TREE/sim/zsim-headless" -r "$CARD" -m 33554432 -n 0 zfpga.bin \
            > "$OUT/$name.out" 2> "$OUT/$name.err"; then
        echo "FAIL $name: simulator"; cat "$OUT/$name.out"; fail=$((fail + 1)); continue
    fi
    ./zfpga pnr "tests/fixtures/$name.zn" -D db -o "$OUT/$name.host.config" > /dev/null
    insns=$(sed -n 's/.*: \([0-9]*\) instructions/\1/p' "$OUT/$name.err")
    if cmp -s "$CARD/$name.config" "$OUT/$name.host.config"; then
        echo "ok   $name  $insns instructions"; pass=$((pass + 1))
    else
        echo "FAIL $name: device and host differ"; cat "$OUT/$name.out"; fail=$((fail + 1))
    fi
done

# Phase 5 on the device: placement is seeded integer annealing, so the
# device must place exactly as the host does.
cp tests/fixtures/blinkf.zl tests/fixtures/dense.zl tests/fixtures/cmp_c.zl tests/fixtures/blink_c.zl "$CARD/"
for name in blinkf dense cmp_c blink_c; do
    echo "place /$name.zl -o /$name.zn" > "$CARD/zfpga.args"
    if ! "$TREE/sim/zsim-headless" -r "$CARD" -m 33554432 -n 0 zfpga.bin \
            > "$OUT/place_$name.out" 2> "$OUT/place_$name.err"; then
        echo "FAIL place $name: simulator"; cat "$OUT/place_$name.out"; fail=$((fail + 1)); continue
    fi
    ./zfpga place "tests/fixtures/$name.zl" -D db -o "$OUT/place_$name.host.zn" > /dev/null
    insns=$(sed -n 's/.*: \([0-9]*\) instructions/\1/p' "$OUT/place_$name.err")
    # line 1 names the input path, which differs; the placement must not
    if [ "$(tail -n +2 "$CARD/$name.zn" | md5sum)" = "$(tail -n +2 "$OUT/place_$name.host.zn" | md5sum)" ]; then
        echo "ok   place $name  $insns instructions"; pass=$((pass + 1))
    else
        echo "FAIL place $name: device and host differ"; cat "$OUT/place_$name.out"; fail=$((fail + 1))
    fi
done

echo
echo "$pass passed, $fail failed"
[ $fail = 0 ]
