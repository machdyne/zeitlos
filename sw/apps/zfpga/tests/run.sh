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

for name in blinkf blink features sidesp adder cmp densen; do
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
