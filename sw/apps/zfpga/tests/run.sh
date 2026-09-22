#!/bin/sh
#
# zfpga differential suite.
#
#   ./tests/run.sh             run everything available
#   ./tests/run.sh --record    rewrite tests/expect.md5 from ecppack
#
# Every case packs a .config with zfpga and with ecppack and requires the
# two bitstreams to be BYTE-IDENTICAL. Differential rather than golden,
# for docs/zcc.md's reason: a golden file records what zfpga did the day
# it was written, and ecppack does not share zfpga's bugs.
#
# Without ecppack on PATH (or with ZFPGA_NO_REF=1), each output is
# checked against tests/expect.md5 instead, recorded from ecppack 1.4.
# That is weaker -- it catches regressions, not original sins -- and the
# summary says which mode ran.
#
# Cases on dies not yet vendored (45F, 85F) need an installed Trellis
# database, $TRELLIS_DB, default /usr/share/trellis/database, and are
# skipped with a note otherwise.
#
# The refusal cases (fixtures/bad_*.config) must exit non-zero, print the
# text on their "# expect:" line, and write no bitstream.

cd "$(dirname "$0")/.." || exit 1

make -s -f Makefile.host || exit 1

OUT=tests/out
TRELLIS_DB=${TRELLIS_DB:-/usr/share/trellis/database}
RECORD=0
[ "$1" = "--record" ] && RECORD=1

rm -rf "$OUT"
mkdir -p "$OUT" "$OUT/hostdb"

LIVE=0
if [ -z "$ZFPGA_NO_REF" ] && command -v ecppack >/dev/null 2>&1; then
    LIVE=1
    echo "reference: $(ecppack --version 2>&1 | head -1)"
else
    echo "reference: recorded md5 (tests/expect.md5); no live ecppack"
fi
if [ $RECORD = 1 ] && [ $LIVE = 0 ]; then
    echo "--record needs ecppack"; exit 1
fi

HOSTDB=0
if [ -f "$TRELLIS_DB/devices.json" ]; then
    for d in 45 85; do
        python3 tools/mkzdb.py --db "$TRELLIS_DB" --device LFE5U-${d}F \
            --source host -o "$OUT/hostdb/lfe5u${d}f.zdb" || exit 1
    done
    HOSTDB=1
fi

for f in tests/fixtures/*.config.gz; do
    gunzip -c "$f" > "$OUT/$(basename "$f" .gz)"
done

pass=0; fail=0; skip=0
[ $RECORD = 1 ] && : > tests/expect.md5

trim() { echo "$1" | sed 's/^ *//; s/ *$//'; }

while IFS='|' read -r name fix zopts eopts dbsel; do
    name=$(trim "$name")
    case "$name" in ''|'#'*) continue ;; esac
    fix=$(trim "$fix"); zopts=$(trim "$zopts"); eopts=$(trim "$eopts"); dbsel=$(trim "$dbsel")

    if [ "$dbsel" = host ]; then
        if [ $HOSTDB = 0 ]; then
            echo "skip $name (no Trellis database at $TRELLIS_DB)"
            skip=$((skip + 1)); continue
        fi
        db=$OUT/hostdb; edb="--db $TRELLIS_DB"
    else
        db=db; edb=""
    fi

    cfg=$OUT/$fix.config
    # shellcheck disable=SC2086
    if ! ./zfpga pack "$cfg" -o "$OUT/$name.bit" -D "$db" $zopts > "$OUT/$name.log" 2>&1; then
        echo "FAIL $name: zfpga failed"; sed 's/^/     /' "$OUT/$name.log"
        fail=$((fail + 1)); continue
    fi

    if [ $LIVE = 1 ]; then
        # shellcheck disable=SC2086
        if ! ecppack $edb $eopts "$cfg" "$OUT/$name.ref.bit" > "$OUT/$name.ref.log" 2>&1; then
            echo "FAIL $name: ecppack failed"; fail=$((fail + 1)); continue
        fi
        if cmp -s "$OUT/$name.bit" "$OUT/$name.ref.bit"; then
            echo "ok   $name ($(wc -c < "$OUT/$name.bit") bytes)"
            pass=$((pass + 1))
            [ $RECORD = 1 ] && (cd "$OUT" && md5sum "$name.bit") >> tests/expect.md5
        else
            n=$(cmp -l "$OUT/$name.bit" "$OUT/$name.ref.bit" 2>/dev/null | wc -l)
            echo "FAIL $name: differs from ecppack ($n bytes; sizes $(wc -c < "$OUT/$name.bit") vs $(wc -c < "$OUT/$name.ref.bit"))"
            fail=$((fail + 1))
        fi
    else
        want=$(grep " $name.bit\$" tests/expect.md5 | cut -d' ' -f1)
        got=$( (cd "$OUT" && md5sum "$name.bit") | cut -d' ' -f1)
        if [ -z "$want" ]; then
            echo "skip $name (no recorded md5)"; skip=$((skip + 1))
        elif [ "$want" = "$got" ]; then
            echo "ok   $name (md5)"; pass=$((pass + 1))
        else
            echo "FAIL $name: md5 $got, recorded $want"; fail=$((fail + 1))
        fi
    fi
done < tests/cases.txt

# Refusals.
for f in tests/fixtures/bad_*.config; do
    name=$(basename "$f" .config)
    want=$(sed -n 's/^# expect: //p' "$f" | head -1)
    rm -f "$OUT/$name.bit"
    if ./zfpga pack "$f" -o "$OUT/$name.bit" -D db > "$OUT/$name.log" 2>&1; then
        echo "FAIL $name: accepted, should have been refused"; fail=$((fail + 1))
    elif [ -e "$OUT/$name.bit" ]; then
        echo "FAIL $name: refused but left $OUT/$name.bit behind"; fail=$((fail + 1))
    elif grep -qF -- "$want" "$OUT/$name.log"; then
        echo "ok   $name: $(head -1 "$OUT/$name.log")"; pass=$((pass + 1))
    else
        echo "FAIL $name: wanted '$want', got:"; sed 's/^/     /' "$OUT/$name.log"
        fail=$((fail + 1))
    fi
done

# -- zfpga unpack: a bitstream back to text --------------------------
#
# Each bitstream above is unpacked; the .config must be the TEXT
# ecpunpack prints for it (live, or its recorded md5), and running the
# `zfpga pack` command unpack prints must rebuild the bitstream byte for
# byte. Then a bitstream with one byte damaged must be refused.

for name in blink blink_c blink_boot blink_qspi blink_user blink_bg dfu_boot edge; do
    bit="$OUT/$name.bit"
    [ -f "$bit" ] || continue
    if ! ./zfpga unpack "$bit" -D db -o "$OUT/unpack_$name.config" > "$OUT/unpack_$name.log" 2>&1; then
        echo "FAIL unpack $name:"; sed 's/^/     /' "$OUT/unpack_$name.log"; fail=$((fail + 1)); continue
    fi
    if [ $LIVE = 1 ]; then
        ecpunpack "$bit" "$OUT/unpack_$name.ref" > /dev/null 2>&1
        cmp -s "$OUT/unpack_$name.config" "$OUT/unpack_$name.ref" && same=1 || same=0
        [ $RECORD = 1 ] && (cd "$OUT" && md5sum "unpack_$name.config") >> tests/expect.md5
    else
        want=$(grep " unpack_$name.config\$" tests/expect.md5 | cut -d' ' -f1)
        got=$( (cd "$OUT" && md5sum "unpack_$name.config") | cut -d' ' -f1)
        [ -n "$want" ] && [ "$want" = "$got" ] && same=1 || same=0
    fi
    cmd=$(sed -n 's/^zfpga: to rebuild this bitstream exactly: zfpga //p' "$OUT/unpack_$name.log")
    # shellcheck disable=SC2086
    if [ $same = 1 ] && ./zfpga $cmd -D db -o "$OUT/unpack_$name.bit" > /dev/null 2>&1 &&
       cmp -s "$OUT/unpack_$name.bit" "$bit"; then
        echo "ok   unpack $name: text as ecpunpack; 'zfpga $cmd' rebuilds it exactly"
        pass=$((pass + 1))
    else
        echo "FAIL unpack $name (text same as ecpunpack: $same; rebuild: zfpga $cmd)"; fail=$((fail + 1))
    fi
done

if [ -f "$OUT/blink.bit" ]; then
    python3 -c "
import sys
d = bytearray(open(sys.argv[1], 'rb').read()); d[len(d) // 2] ^= 0x10
open(sys.argv[2], 'wb').write(d)" "$OUT/blink.bit" "$OUT/damaged.bit"
    rm -f "$OUT/damaged.config"
    if ./zfpga unpack "$OUT/damaged.bit" -D db -o "$OUT/damaged.config" > "$OUT/damaged.log" 2>&1; then
        echo "FAIL damaged bitstream accepted"; fail=$((fail + 1))
    elif [ -e "$OUT/damaged.config" ]; then
        echo "FAIL damaged bitstream left output"; fail=$((fail + 1))
    elif grep -q "CRC mismatch" "$OUT/damaged.log"; then
        echo "ok   damaged bitstream: $(head -1 "$OUT/damaged.log" | sed 's|.*: CRC|CRC|')"; pass=$((pass + 1))
    else
        echo "FAIL damaged bitstream:"; cat "$OUT/damaged.log"; fail=$((fail + 1))
    fi
fi

# -- Phase 3: zfpga pnr against nextpnr ------------------------------
#
# Each .zn is translated by zfpga pnr and packed; the nextpnr .config
# for the same placed design is packed too, and the two bitstreams must
# be identical. zfpga pack is itself checked against ecppack above, so
# this needs no external tool: the references are fixtures.
#
#   blink   examples/blink.zn, converted from nextpnr by tools/np2zn.py
#   on      examples/on.zn, WRITTEN BY HAND
#   sides   IO on all four sides, two banks, a tristate, per-pin options,
#           async reset and clock enable (examples/sides.v)

for spec in "blink:examples/blink.zn:blink" "on:examples/on.zn:on" \
        "sides:tests/fixtures/sides.zn:sides"; do
    name=${spec%%:*}; rest=${spec#*:}; zn=${rest%%:*}; ref=${rest#*:}
    if ./zfpga pnr "$zn" -D db -o "$OUT/pnr_$name.config" > "$OUT/pnr_$name.log" 2>&1 &&
       ./zfpga pack "$OUT/pnr_$name.config" -D db -o "$OUT/pnr_$name.bit" >> "$OUT/pnr_$name.log" 2>&1 &&
       gunzip -c "tests/fixtures/$ref.config.gz" > "$OUT/pnr_$name.ref.config" &&
       ./zfpga pack "$OUT/pnr_$name.ref.config" -D db -o "$OUT/pnr_$name.ref.bit" >> "$OUT/pnr_$name.log" 2>&1; then
        if cmp -s "$OUT/pnr_$name.bit" "$OUT/pnr_$name.ref.bit"; then
            echo "ok   pnr $name == nextpnr"
            pass=$((pass + 1))
        else
            echo "FAIL pnr $name: differs from nextpnr ($(cmp -l "$OUT/pnr_$name.bit" "$OUT/pnr_$name.ref.bit" | wc -l) bytes)"
            fail=$((fail + 1))
        fi
    else
        echo "FAIL pnr $name:"; sed 's/^/     /' "$OUT/pnr_$name.log"; fail=$((fail + 1))
    fi
done

# -- Phase 4: the router ----------------------------------------------
#
# Nets declared by their terminals, routed by zfpga, then checked by
# tools/routecheck.py -- independent of route.c, sharing only the
# resolver that tools/resolvecheck.py proves exact against nextpnr --
# and packed. Routes differ from nextpnr's, so the test is legality:
# every sink reached, no wire with two drivers. Needs python3.

for name in on blink sides dense; do
    zn=tests/fixtures/${name}_route.zn
    if ./zfpga pnr "$zn" -D db -o "$OUT/route_$name.config" > "$OUT/route_$name.log" 2>&1 &&
       python3 tools/routecheck.py --db ext/prjtrellis-db --device LFE5U-25F \
            --base ext/nextpnr-base/lfe5u-25f.config "$zn" "$OUT/route_$name.config" \
            >> "$OUT/route_$name.log" 2>&1 &&
       ./zfpga pack "$OUT/route_$name.config" -D db -o "$OUT/route_$name.bit" >> "$OUT/route_$name.log" 2>&1; then
        echo "ok   route $name: $(grep '^zfpga: routed' "$OUT/route_$name.log" | sed 's/^zfpga: //'); legal"
        pass=$((pass + 1))
    else
        echo "FAIL route $name:"; sed 's/^/     /' "$OUT/route_$name.log"; fail=$((fail + 1))
    fi
done

# -- Phase 5: place, route, and check the result computes the design ---
#
# From a logical netlist (.zl, what synthesis produces) with no nextpnr
# anywhere: zfpga place, zfpga pnr, then two independent checks --
# routecheck.py for legality and simcheck.py, which extracts the circuit
# from the .config alone and simulates it against the .zl, cycle by
# cycle (docs/zfpga.md sec. 15.4) -- then zfpga pack.

for name in blinkf sidesp dense blinkf_c blink_c dense_c adder_c cmp_c; do
    zl=tests/fixtures/$name.zl
    log="$OUT/place_$name.log"
    if ./zfpga place "$zl" -D db -o "$OUT/place_$name.zn" > "$log" 2>&1 &&
       ./zfpga pnr "$OUT/place_$name.zn" -D db -o "$OUT/place_$name.config" >> "$log" 2>&1 &&
       python3 tools/routecheck.py --db ext/prjtrellis-db --device LFE5U-25F \
            --base ext/nextpnr-base/lfe5u-25f.config "$OUT/place_$name.zn" \
            "$OUT/place_$name.config" >> "$log" 2>&1 &&
       python3 tools/simcheck.py --db ext/prjtrellis-db --device LFE5U-25F \
            --package CABGA256 "$zl" - "$OUT/place_$name.config" >> "$log" 2>&1 &&
       ./zfpga pack "$OUT/place_$name.config" -D db -o "$OUT/place_$name.bit" >> "$log" 2>&1; then
        echo "ok   place $name: $(grep -o 'in [0-9]* slices; wirelength [0-9]* -> [0-9]*' "$log"); legal; $(grep -o 'outputs changed on [0-9]*' "$log"); equivalent"
        pass=$((pass + 1))
    else
        echo "FAIL place $name:"; sed 's/^/     /' "$log"; fail=$((fail + 1))
    fi
done

# Rung 1 has no nextpnr reference (nextpnr refuses a design with no
# cells). It must be exactly the baseline plus the Part comment.
{ echo ".device LFE5U-25F"; echo ".comment Part: LFE5U-25F-6CABGA256";
  grep -v '^\.device' ext/nextpnr-base/lfe5u-25f.config; } > "$OUT/pnr_empty.ref.config"
if ./zfpga pnr examples/empty.zn -D db -o "$OUT/pnr_empty.config" > /dev/null 2>&1 &&
   ./zfpga pack "$OUT/pnr_empty.config" -D db -o "$OUT/pnr_empty.bit" > /dev/null 2>&1 &&
   ./zfpga pack "$OUT/pnr_empty.ref.config" -D db -o "$OUT/pnr_empty.ref.bit" > /dev/null 2>&1 &&
   cmp -s "$OUT/pnr_empty.bit" "$OUT/pnr_empty.ref.bit"; then
    echo "ok   pnr empty == baseline"; pass=$((pass + 1))
else
    echo "FAIL pnr empty"; fail=$((fail + 1))
fi

for f in tests/fixtures/badzn_*.zn; do
    name=$(basename "$f" .zn)
    want=$(sed -n 's/^# expect: //p' "$f" | head -1)
    rm -f "$OUT/$name.config"
    if ./zfpga pnr "$f" -D db -o "$OUT/$name.config" > "$OUT/$name.log" 2>&1; then
        echo "FAIL $name: accepted, should have been refused"; fail=$((fail + 1))
    elif [ -e "$OUT/$name.config" ]; then
        echo "FAIL $name: refused but left output behind"; fail=$((fail + 1))
    elif grep -qF -- "$want" "$OUT/$name.log"; then
        echo "ok   $name: $(head -1 "$OUT/$name.log")"; pass=$((pass + 1))
    else
        echo "FAIL $name: wanted '$want', got:"; sed 's/^/     /' "$OUT/$name.log"
        fail=$((fail + 1))
    fi
done

# -- Phase 6: zfpga synth, Verilog to bitstream with no host tool ------
#
# Each design is synthesised by zfpga and checked against yosys's
# netlist of the same Verilog (tests/fixtures/*_c.zl, from yosys via
# tools/ys2zl.py): first the .zl, then -- through zfpga place, pnr and
# pack -- the final configuration.

for name in blinkf blink features sidesp adder cmp densen hier loops mem signed func partsel blocking; do
    ref=tests/fixtures/${name}_c.zl
    log="$OUT/synth_$name.log"
    lpf=examples/$name.lpf
    [ -f "$lpf" ] || lpf=examples/blink_lakritz.lpf         # blink, blinkf
    if ./zfpga synth examples/$name.v -l "$lpf" -o "$OUT/synth_$name.zl" > "$log" 2>&1 &&
       python3 tools/simcheck.py --db ext/prjtrellis-db --device LFE5U-25F --package CABGA256 \
            "$ref" - "$OUT/synth_$name.zl" >> "$log" 2>&1 &&
       ./zfpga place "$OUT/synth_$name.zl" -D db -o "$OUT/synth_$name.zn" >> "$log" 2>&1 &&
       ./zfpga pnr "$OUT/synth_$name.zn" -D db -o "$OUT/synth_$name.config" >> "$log" 2>&1 &&
       python3 tools/simcheck.py --db ext/prjtrellis-db --device LFE5U-25F --package CABGA256 \
            "$ref" - "$OUT/synth_$name.config" >> "$log" 2>&1 &&
       ./zfpga pack "$OUT/synth_$name.config" -D db -c -o "$OUT/synth_$name.bit" >> "$log" 2>&1; then
        echo "ok   synth $name: $(grep -o 'synthesised.*' "$log" | head -1); .zl and bitstream equivalent to yosys"
        pass=$((pass + 1))
    else
        echo "FAIL synth $name:"; sed 's/^/     /' "$log"; fail=$((fail + 1))
    fi
done

for f in tests/fixtures/badv_*.v; do
    name=$(basename "$f" .v)
    want=$(sed -n 's|^// expect: ||p' "$f" | head -1)
    rm -f "$OUT/$name.zl"
    if ./zfpga synth "$f" -l examples/blink_lakritz.lpf -o "$OUT/$name.zl" > "$OUT/$name.log" 2>&1; then
        echo "FAIL $name: accepted, should have been refused"; fail=$((fail + 1))
    elif [ -e "$OUT/$name.zl" ]; then
        echo "FAIL $name: refused but left output behind"; fail=$((fail + 1))
    elif grep -qF -- "$want" "$OUT/$name.log"; then
        echo "ok   $name: $(head -1 "$OUT/$name.log" | sed 's|^tests/fixtures/||')"; pass=$((pass + 1))
    else
        echo "FAIL $name: wanted '$want', got:"; sed 's/^/     /' "$OUT/$name.log"; fail=$((fail + 1))
    fi
done

# -- The card's /fpga: every name 8.3 --------------------------------
#
# db/ is laid out as /fpga on the card, whose FatFs has no long names.
# A name that does not fit is unreadable there however well it reads
# here: lfe5u-25f.zdb was the first on-board run's "no database".

bad83=$(cd db && find . -mindepth 1 | sed 's|^\./||' | while read -r p; do
    echo "$p" | tr '/' '\n' | while read -r c; do
        stem=${c%%.*}; ext=""; [ "$stem" != "$c" ] && ext=${c#*.}
        if [ ${#stem} -gt 8 ] || [ ${#ext} -gt 3 ] || [ -z "$stem" ]; then echo "$p"; fi
    done
done | sort -u)
if [ -z "$bad83" ]; then
    echo "ok   db/ (the card's /fpga): $(cd db && find . -type f | wc -l) files, every name 8.3"; pass=$((pass + 1))
else
    echo "FAIL db/ has names that are not 8.3:"; echo "$bad83" | sed 's/^/     /'; fail=$((fail + 1))
fi

# -- zfpga build: the board profile, every stage in one process -------
#
# From each starting point, `zfpga build -b lakritz` must make the same
# bitstream as running the stages by hand with the profile's settings,
# and the Verilog one must still compute what yosys says it does.

mkdir -p "$OUT/build"
cp db/examples/blink.v db/examples/on.zn db/examples/hand.zl "$OUT/build/"
if ./zfpga build "$OUT/build/on.zn" -b lakritz -D db > "$OUT/build.log" 2>&1 &&
   ./zfpga pnr db/examples/on.zn -D db -o "$OUT/build/on.manual.config" > /dev/null 2>&1 &&
   ./zfpga pack "$OUT/build/on.manual.config" -D db -c -o "$OUT/build/on.manual.bit" > /dev/null 2>&1 &&
   cmp -s "$OUT/build/on.bit" "$OUT/build/on.manual.bit"; then
    echo "ok   build on.zn -b lakritz: the same bitstream as pnr + pack -c by hand"; pass=$((pass + 1))
else
    echo "FAIL build on.zn:"; cat "$OUT/build.log"; fail=$((fail + 1))
fi
cp db/examples/blinkh.v db/examples/blinkh.vh "$OUT/build/"
if ./zfpga build "$OUT/build/blinkh.v" -b lakritz -D db > "$OUT/buildh.log" 2>&1 &&
   grep -q "1 instance flattened" "$OUT/buildh.log" && grep -q "25 flip-flops" "$OUT/buildh.log"; then
    echo "ok   build blinkh.v -b lakritz: an instance, a parameter, an \`include: 25 flip-flops"
    pass=$((pass + 1))
else
    echo "FAIL build blinkh.v:"; sed 's/^/     /' "$OUT/buildh.log"; fail=$((fail + 1))
fi
for f in blink.v hand.zl; do
    ref=tests/fixtures/blink_c.zl
    [ $f = hand.zl ] && ref=examples/hand.zl
    if ./zfpga build "$OUT/build/$f" -b lakritz -D db > "$OUT/build.log" 2>&1 &&
       [ -s "$OUT/build/${f%.*}.bit" ] &&
       python3 tools/simcheck.py --db ext/prjtrellis-db --device LFE5U-25F --package CABGA256 \
            "$ref" - "$OUT/build/${f%.*}.cfg" >> "$OUT/build.log" 2>&1; then
        echo "ok   build $f -b lakritz: $(grep -o 'peak memory [0-9]* KB' "$OUT/build.log"); equivalent"
        pass=$((pass + 1))
    else
        echo "FAIL build $f:"; sed 's/^/     /' "$OUT/build.log"; fail=$((fail + 1))
    fi
done

# -- zfpga bram: block RAM contents, as ecpbram replaces them --------
#
# tests/fixtures/bram: a ROM initialised from a random seed, placed and
# routed by nextpnr (rom.v, rom.config); the seed, and new contents.
# The .cfg route must pack to what ecpbram's output packs to (ecpbram
# re-serialises the file and duplicates a tile in each .tile_group, so
# the text is compared through the bitstream); the .bit route, from a
# compressed multiboot bitstream, must give what the .cfg route gives,
# packed with the same options; -g -s must write ecpbram's file.

BR=tests/fixtures/bram
if ./zfpga bram $BR/rom.config -f $BR/seed.hex -t $BR/new.hex -o "$OUT/bram.cfg" > "$OUT/bram.log" 2>&1 &&
   ./zfpga pack "$OUT/bram.cfg" -D db -o "$OUT/bram.bit" >> "$OUT/bram.log" 2>&1; then
    if [ $LIVE = 1 ]; then
        ecpbram -i $BR/rom.config -o "$OUT/bram.ref.config" -f $BR/seed.hex -t $BR/new.hex &&
        ecppack "$OUT/bram.ref.config" "$OUT/bram.ref.bit" && cmp -s "$OUT/bram.bit" "$OUT/bram.ref.bit" && same=1 || same=0
        [ $RECORD = 1 ] && (cd "$OUT" && md5sum bram.bit) >> tests/expect.md5
    else
        want=$(grep " bram.bit\$" tests/expect.md5 | cut -d' ' -f1)
        [ -n "$want" ] && [ "$want" = "$( (cd "$OUT" && md5sum bram.bit) | cut -d' ' -f1)" ] && same=1 || same=0
    fi
    if [ $same = 1 ]; then
        echo "ok   bram .cfg: $(grep -o '[0-9]* block RAMs, [0-9]* slices replaced' "$OUT/bram.log"); packs as ecpbram's does"
        pass=$((pass + 1))
    else
        echo "FAIL bram .cfg: not the bitstream ecpbram's output packs to"; fail=$((fail + 1))
    fi
else
    echo "FAIL bram .cfg:"; sed 's/^/     /' "$OUT/bram.log"; fail=$((fail + 1))
fi
if ./zfpga pack $BR/rom.config -D db -c -a 0x040000 -o "$OUT/bram_in.bit" > /dev/null 2>&1 &&
   ./zfpga bram "$OUT/bram_in.bit" -f $BR/seed.hex -t $BR/new.hex -o "$OUT/bram_out.bit" -D db > "$OUT/bram2.log" 2>&1 &&
   ./zfpga pack "$OUT/bram.cfg" -D db -c -a 0x040000 -o "$OUT/bram_want.bit" > /dev/null 2>&1 &&
   cmp -s "$OUT/bram_out.bit" "$OUT/bram_want.bit" && [ ! -e "$OUT/bram_out.b1" ] && [ ! -e "$OUT/bram_out.b2" ]; then
    echo "ok   bram .bit (compressed, multiboot at 0x040000): the same bitstream, options kept"; pass=$((pass + 1))
else
    echo "FAIL bram .bit:"; sed 's/^/     /' "$OUT/bram2.log"; fail=$((fail + 1))
fi
./zfpga bram -g "$OUT/seed_z.hex" -w 32 -d 1024 -s 1234 > /dev/null 2>&1
if [ $LIVE = 1 ]; then
    ecpbram -g "$OUT/seed_e.hex" -w 32 -d 1024 -s 1234 && cmp -s "$OUT/seed_z.hex" "$OUT/seed_e.hex" && same=1 || same=0
    [ $RECORD = 1 ] && (cd "$OUT" && md5sum seed_z.hex) >> tests/expect.md5
else
    want=$(grep " seed_z.hex\$" tests/expect.md5 | cut -d' ' -f1)
    [ -n "$want" ] && [ "$want" = "$( (cd "$OUT" && md5sum seed_z.hex) | cut -d' ' -f1)" ] && same=1 || same=0
fi
if [ $same = 1 ]; then echo "ok   bram -g -s 1234: ecpbram's seed file, byte for byte"; pass=$((pass + 1))
else echo "FAIL bram -g -s 1234"; fail=$((fail + 1)); fi
head -512 $BR/seed.hex > "$OUT/half.hex"; cat "$OUT/half.hex" "$OUT/half.hex" > "$OUT/twice.hex"
if ./zfpga bram $BR/rom.config -f "$OUT/twice.hex" -t $BR/new.hex -o "$OUT/x.cfg" > "$OUT/bram3.log" 2>&1; then
    echo "FAIL bram: a repeating seed accepted"; fail=$((fail + 1))
elif grep -q "the seed must be random" "$OUT/bram3.log"; then
    echo "ok   bram: a repeating seed refused"; pass=$((pass + 1))
else
    echo "FAIL bram repeating seed:"; cat "$OUT/bram3.log"; fail=$((fail + 1))
fi

# -- zfpga jump: jumploaders (docs/zboot.md sec. 5) -------------------
#
# One for each board with PROGRAMN, to 0 and to 0x190000; the same
# length for every target (the point of -J); with the Trellis tools,
# ecpunpack checks every CRC and reads back the boot address. Then the
# kernel's own code (sw/os/jumpapi.c, sw/common/zjump.c) against a
# simulated flash: re-pointing one gives the other, byte for byte --
# once as built, once shifted across a sector boundary.

jump_ok=1
for b in lakritz obst; do
    for t in 0x000000 0x190000; do
        if ! ./zfpga jump $t -b $b -D db -o "$OUT/j_${b}_$t.bit" > "$OUT/jump.log" 2>&1; then
            echo "FAIL jump $t -b $b:"; sed 's/^/     /' "$OUT/jump.log"; jump_ok=0; fail=$((fail + 1))
        fi
    done
done
if [ $jump_ok = 1 ]; then
    n0=$(wc -c < "$OUT/j_lakritz_0x000000.bit"); n1=$(wc -c < "$OUT/j_lakritz_0x190000.bit")
    if [ "$n0" = "$n1" ] && [ ! -e "$OUT/j_lakritz_0x000000.v" ]; then
        echo "ok   jump -b lakritz, -b obst: $n0 bytes, the same for every target"; pass=$((pass + 1))
    else
        echo "FAIL jump: $n0 and $n1 bytes, or its source was left behind"; fail=$((fail + 1))
    fi
    if [ $LIVE = 1 ]; then
        good=1
        for b in lakritz obst; do
            for t in 0x000000 0x190000; do
                if ! ecpunpack "$OUT/j_${b}_$t.bit" "$OUT/j.txt" > /dev/null 2>&1; then good=0
                elif [ $t = 0x190000 ] && ! grep -q "BOOTADDR 00011001" "$OUT/j.txt"; then good=0; fi
            done
        done
        if [ $good = 1 ]; then echo "ok   ecpunpack reads all four: every CRC, and the boot address"; pass=$((pass + 1))
        else echo "FAIL ecpunpack rejects a jumploader"; fail=$((fail + 1)); fi
    fi
    if cc -O1 -Wall -I../../os -I../../common -o "$OUT/kjump" tests/kjump.c ../../os/jumpapi.c ../../common/zjump.c \
            > "$OUT/kjump.log" 2>&1 &&
       "$OUT/kjump" "$OUT/j_lakritz_0x000000.bit" "$OUT/j_lakritz_0x190000.bit" >> "$OUT/kjump.log" 2>&1; then
        echo "ok   the kernel's jumploader code: $(grep -c '^ok' "$OUT/kjump.log") checks, one sector and two"; pass=$((pass + 1))
    else
        echo "FAIL the kernel's jumploader code:"; grep -v '^ok\|^jump:\|^--' "$OUT/kjump.log" | sed 's/^/     /'; fail=$((fail + 1))
    fi
fi

# -- zfpga flash / run (docs/zboot.md sec. 5) --------------------------
#
# The real commands against flash image files (zfpga-simflash: the
# hardware's rules -- erase to FF, program clears bits, nothing below
# 0x040000). A Lakritz image: core apps ending at 0x186000, the
# jumploader at 0x1D0000.

F="$OUT/fl"; mkdir -p "$F"
SF=./zfpga-simflash
same() { cmp -s "$1" "$2"; }
at() { dd if="$1" bs=1 skip=$(($2)) count=$3 2>/dev/null; }        # image, offset, length
flash_ok=1
fl_fail() { echo "FAIL flash/run: $1"; flash_ok=0; }
# built from a copy: zfpga build writes its intermediates beside its input
cp examples/blink.v "$F/blink.v"
if make -s -f Makefile.host zfpga-simflash > "$F/mk.log" 2>&1 &&
   ./zfpga build "$F/blink.v" -b lakritz -D db -o "$F/blink.bit" > /dev/null 2>&1 &&
   ./zfpga build "$F/blink.v" -b mozart_ml1 -D db -o "$F/b45.bit" > /dev/null 2>&1 &&
   ./zfpga jump 0 -b lakritz -D db -o "$F/jl.bit" > /dev/null 2>&1; then
    n=$(wc -c < "$F/blink.bit")
    python3 tests/mkflash.py "$F/img" 0x200000 0x186000 "$F/jl.bit"
    cp "$F/img" "$F/img0"
    ZFPGA_SIMFLASH="$F/img" $SF flash "$F/blink.bit" > "$F/o1" 2>&1 &&
        grep -q "at 0x190000" "$F/o1" && grep -q "verified" "$F/o1" &&
        [ "$(at "$F/img" 0x190000 $n | md5sum)" = "$(md5sum < "$F/blink.bit")" ] ||
        fl_fail "the default place, after the core apps: $(cat "$F/o1")"
    cmp -l "$F/img0" "$F/img" | awk -v lo=$((0x190000)) -v hi=$((0x190000 + n)) \
        '$1 - 1 < lo || $1 - 1 >= hi { bad = 1 } END { exit bad }' ||
        fl_fail "bytes outside the image changed"
    cp "$F/img" "$F/img1"
    ZFPGA_SIMFLASH="$F/img" $SF run "$F/blink.bit" > "$F/o2" 2>&1 &&
        grep -q "already at 0x190000" "$F/o2" && same "$F/img" "$F/img1" &&
        [ "$(cat "$F/img.jump")" = "0x190000" ] ||
        fl_fail "run, again: no write, and a jump to 0x190000: $(cat "$F/o2")"
    ZFPGA_SIMFLASH="$F/img" $SF flash "$F/b45.bit" > "$F/o3" 2>&1 &&
        fl_fail "a 45F bitstream accepted on a 25F"
    grep -q "this FPGA is" "$F/o3" && same "$F/img" "$F/img1" || fl_fail "... refused for the wrong reason: $(cat "$F/o3")"
    for bad in "-a 0x191000:not 64 KB aligned" "-a 0x140000:is not free" "-a 0x200000:nothing after the jumploader" \
               "-a 0x000000:is not free"; do
        ZFPGA_SIMFLASH="$F/img" $SF flash "$F/blink.bit" ${bad%%:*} > "$F/o4" 2>&1 && fl_fail "${bad%%:*} accepted"
        grep -q "${bad#*:}" "$F/o4" && same "$F/img" "$F/img1" || fl_fail "${bad%%:*}: $(cat "$F/o4")"
    done
    ZFPGA_SIMFLASH="$F/img" $SF flash tests/run.sh > "$F/o5" 2>&1 && fl_fail "a text file accepted"
    grep -q "not an ECP5 bitstream" "$F/o5" || fl_fail "a text file: $(cat "$F/o5")"
    python3 tests/mkflash.py "$F/full" 0x200000 0x1C8000 "$F/jl.bit"
    ZFPGA_SIMFLASH="$F/full" $SF flash "$F/blink.bit" > "$F/o6" 2>&1 && fl_fail "too big below the jumploader, accepted"
    grep -q "do not fit" "$F/o6" || fl_fail "too big: $(cat "$F/o6")"
    python3 tests/mkflash.py "$F/big" 0x400000 0x1C8000 "$F/jl.bit"
    ZFPGA_SIMFLASH="$F/big" $SF flash "$F/blink.bit" > "$F/o7" 2>&1; grep -q "\-a 0x200000 puts it after" "$F/o7" ||
        fl_fail "a 4 MB flash: no hint to go after the jumploader: $(cat "$F/o7")"
    ZFPGA_SIMFLASH="$F/big" $SF run "$F/blink.bit" -a 0x200000 > "$F/o8" 2>&1 &&
        [ "$(at "$F/big" 0x200000 $n | md5sum)" = "$(md5sum < "$F/blink.bit")" ] &&
        [ "$(cat "$F/big.jump")" = "0x200000" ] || fl_fail "-a 0x200000 on 4 MB: $(cat "$F/o8")"
    python3 tests/mkflash.py "$F/nojl" 0x200000 0x186000
    ZFPGA_SIMFLASH="$F/nojl" $SF run "$F/blink.bit" > "$F/o9" 2>&1 && fl_fail "run with no jumploader, accepted"
    grep -q "no jumploader" "$F/o9" || fl_fail "run with no jumploader: $(cat "$F/o9")"
    ./zfpga build "$F/blink.v" -b lakritz -D db -o "$F/own.bit" -- -a 0x190000 > /dev/null 2>&1
    ZFPGA_SIMFLASH="$F/img" $SF flash "$F/own.bit" -a 0x1A0000 > "$F/o10" 2>&1
    grep -q "its own boot address" "$F/o10" || fl_fail "no warning for a bitstream with its own boot address: $(cat "$F/o10")"
    ./zfpga flash "$F/blink.bit" > "$F/o11" 2>&1 && fl_fail "the host zfpga wrote a flash"
    grep -q "works on the machine" "$F/o11" || fl_fail "the host zfpga: $(cat "$F/o11")"
    if [ $flash_ok = 1 ]; then
        echo "ok   flash / run: placed after the core apps, verified, no rewrite of the same image, a jump;"
        echo "     refused: wrong die, not free, unaligned, too big, not a bitstream, no jumploader; -a after it on 4 MB"
        pass=$((pass + 1))
    else fail=$((fail + 1)); fi
else
    echo "FAIL flash/run: could not build the test inputs"; cat "$F/mk.log"; fail=$((fail + 1))
fi

# -- Every source file is one git would keep --------------------------
#
# The tree's .gitignore ignores *.config and *.json; here those are the
# vendored database, the nextpnr baseline and test fixtures, and the
# first commit of zfpga lost all sixteen (sw/apps/zfpga/.gitignore now
# re-includes them). In a git checkout, any file here git would ignore,
# other than build products, fails.

if git rev-parse --is-inside-work-tree > /dev/null 2>&1; then
    lost=$(find . -type f ! -path './build/*' ! -path './db/*' ! -path './tests/out/*' \
        ! -name zfpga ! -name 'zfpga.*' ! -path '*/__pycache__/*' | sed 's|^\./||' |
        git check-ignore --stdin 2>/dev/null)
    if [ -z "$lost" ]; then
        echo "ok   every source file here is one git keeps"; pass=$((pass + 1))
    else
        echo "FAIL git would not commit these source files:"; echo "$lost" | sed 's/^/     /'; fail=$((fail + 1))
    fi
    # zfpga build writes intermediates beside its input: a test that
    # builds a tracked example in place overwrites it (one did, and
    # replaced the hand-written examples/blink.zn)
    touched=$( (git diff --name-only -- examples; git ls-files --others --exclude-standard -- examples) 2>/dev/null)
    if [ -z "$touched" ]; then
        echo "ok   the tests left examples/ as git has it"; pass=$((pass + 1))
    else
        echo "FAIL the tests changed examples/:"; echo "$touched" | sed 's/^/     /'; fail=$((fail + 1))
    fi
fi

# -- Manual placement: loc= and the -l round trip (docs/zfpga-formats.md)
#
# examples/hand.zl is written by hand, with func= LUTs and two cells
# pinned by loc=; it must place with those two exactly where it says,
# route, and compute what it says. Then the dense design is placed with
# -l, which writes every cell's loc= back; placing that file again must
# reproduce the placement exactly.

log="$OUT/hand.log"
if ./zfpga place examples/hand.zl -D db -o "$OUT/hand.zn" > "$log" 2>&1 &&
   grep -q "^ff R2C4 SLICEA 0 " "$OUT/hand.zn" && grep -q "^ff R2C4 SLICEA 1 " "$OUT/hand.zn" &&
   ./zfpga pnr "$OUT/hand.zn" -D db -o "$OUT/hand.config" >> "$log" 2>&1 &&
   python3 tools/routecheck.py --db ext/prjtrellis-db --device LFE5U-25F \
        --base ext/nextpnr-base/lfe5u-25f.config "$OUT/hand.zn" "$OUT/hand.config" >> "$log" 2>&1 &&
   python3 tools/simcheck.py --db ext/prjtrellis-db --device LFE5U-25F --package CABGA256 \
        examples/hand.zl - "$OUT/hand.config" >> "$log" 2>&1; then
    echo "ok   hand.zl: loc= honoured; legal; $(grep -o 'outputs changed on [0-9]*' "$log"); equivalent"
    pass=$((pass + 1))
else
    echo "FAIL hand.zl:"; sed 's/^/     /' "$log"; fail=$((fail + 1))
fi

if ./zfpga place tests/fixtures/dense_c.zl -D db -o "$OUT/rt1.zn" -l "$OUT/rt.zl" > /dev/null 2>&1 &&
   ./zfpga place "$OUT/rt.zl" -D db -o "$OUT/rt2.zn" > "$OUT/rt.log" 2>&1 &&
   [ "$(tail -n +2 "$OUT/rt1.zn" | md5sum)" = "$(tail -n +2 "$OUT/rt2.zn" | md5sum)" ]; then
    echo "ok   -l round trip: $(grep -c 'loc=' "$OUT/rt.zl") loc= written back; re-placed identically"
    pass=$((pass + 1))
else
    echo "FAIL -l round trip"; fail=$((fail + 1))
fi

for f in tests/fixtures/badzl_*.zl; do
    name=$(basename "$f" .zl)
    want=$(sed -n 's/^# expect: //p' "$f" | head -1)
    rm -f "$OUT/$name.zn"
    if ./zfpga place "$f" -D db -o "$OUT/$name.zn" > "$OUT/$name.log" 2>&1; then
        echo "FAIL $name: accepted, should have been refused"; fail=$((fail + 1))
    elif [ -e "$OUT/$name.zn" ]; then
        echo "FAIL $name: refused but left output behind"; fail=$((fail + 1))
    elif grep -qF -- "$want" "$OUT/$name.log"; then
        echo "ok   $name: $(head -1 "$OUT/$name.log")"; pass=$((pass + 1))
    else
        echo "FAIL $name: wanted '$want', got:"; sed 's/^/     /' "$OUT/$name.log"; fail=$((fail + 1))
    fi
done

# Bad options: refused before anything is written.
for spec in "-m warp:bad SPI mode" "-f 3.0:bad frequency" "-a 0x12345:not 64K aligned"; do
    opt=${spec%%:*}; want=${spec#*:}
    rm -f "$OUT/opt.bit"
    # shellcheck disable=SC2086
    if ./zfpga pack "$OUT/blink.config" -o "$OUT/opt.bit" -D db $opt > "$OUT/opt.log" 2>&1; then
        echo "FAIL option $opt accepted"; fail=$((fail + 1))
    elif [ -e "$OUT/opt.bit" ]; then
        echo "FAIL option $opt left a partial $OUT/opt.bit"; fail=$((fail + 1))
    elif grep -qF -- "$want" "$OUT/opt.log"; then
        echo "ok   option $opt: $(head -1 "$OUT/opt.log")"; pass=$((pass + 1))
    else
        echo "FAIL option $opt:"; cat "$OUT/opt.log"; fail=$((fail + 1))
    fi
done

# A damaged database must be refused, not read past its end.
mkdir -p "$OUT/trunc"
head -c 200000 db/lfe5u25f.zdb > "$OUT/trunc/lfe5u25f.zdb"
if ./zfpga info LFE5U-25F -D "$OUT/trunc" > "$OUT/trunc.log" 2>&1; then
    echo "FAIL truncated database accepted"; fail=$((fail + 1))
elif grep -q "truncated" "$OUT/trunc.log"; then
    echo "ok   truncated database: $(head -1 "$OUT/trunc.log")"; pass=$((pass + 1))
else
    echo "FAIL truncated database:"; cat "$OUT/trunc.log"; fail=$((fail + 1))
fi

echo
echo "$pass passed, $fail failed, $skip skipped"
[ $fail = 0 ]
