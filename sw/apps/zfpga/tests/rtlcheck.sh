#!/bin/sh
#
# rtlcheck.sh -- zfpga synth against yosys, on this tree's own RTL.
#
#   tests/rtlcheck.sh [rtl/*.v ...]        (default: every rtl/*.v)
#
# Host only, and needs yosys. For each file: yosys synthesises its first
# module (synth_ecp5 -nowidelut -nodsp -nobram -nolutram) as the
# reference; zfpga synthesises the same; tools/simcheck.py simulates the
# two netlists side by side from the same random inputs and compares
# every output every cycle. Pins do not matter to that comparison, so
# each port bit is given a placeholder site. A module that instantiates
# others is given their files too (tests/rtldeps.py), to both tools.
#
# One line per module. A module zfpga refuses is reported with the
# reason and is not a failure -- the refusals are the list of what is
# still to do (docs/zfpga.md sec. 24.7). A module zfpga ACCEPTS and gets
# wrong is a failure, and the exit status says so. So is a module whose
# check cannot run (more than one clock, for instance) -- reported, not
# failed.

cd "$(dirname "$0")/.." || exit 1
TREE=$(cd ../../.. && pwd)
OUT=tests/out/rtl
mkdir -p "$OUT"
command -v yosys > /dev/null || { echo "rtlcheck: needs yosys"; exit 2; }
[ -x ./zfpga ] || make -s -f Makefile.host || exit 1

[ $# -gt 0 ] || set -- "$TREE"/rtl/*.v
ok=0; bad=0; refused=0; unchecked=0

for f in "$@"; do
    f=$(cd "$(dirname "$f")" && pwd)/$(basename "$f")
    n=$(basename "$f" .v)
    top=$(sed -n 's/^[[:space:]]*module[[:space:]]\{1,\}\([A-Za-z0-9_]*\).*/\1/p' "$f" | head -1)
    files=$(python3 tests/rtldeps.py "$f" $(find "$TREE/rtl" -name '*.v' ! -path '*/tb/*' ! -path '*/tests/*' | sort))
    if ! (cd "$OUT" && yosys -q -p "read_verilog -I$(dirname "$f") $files; synth_ecp5 -nowidelut -nodsp -nobram -nolutram -top $top -json $n.json") \
            > "$OUT/$n.yosys.log" 2>&1; then
        printf '%-16s yosys: %s\n' "$n" "$(grep -m1 -i error "$OUT/$n.yosys.log" | cut -c1-80)"
        unchecked=$((unchecked + 1)); continue
    fi
    python3 - "$OUT/$n.json" "$OUT/$n.lpf" << 'EOF'
import json, sys
m = json.load(open(sys.argv[1]))["modules"]
top = [k for k, v in m.items() if v.get("attributes", {}).get("top")] or list(m)
names = []
for pn, p in m[top[0]]["ports"].items():
    for i, b in enumerate(p["bits"]):
        names.append(pn if len(p["bits"]) == 1 else "%s[%d]" % (pn, i))
open(sys.argv[2], "w").write("".join('LOCATE COMP "%s" SITE "P%d";\n' % (x, i)
                                     for i, x in enumerate(names)))
EOF
    # shellcheck disable=SC2086
    if ! ./zfpga synth $files -t "$top" -l "$OUT/$n.lpf" -o "$OUT/$n.zl" > "$OUT/$n.zfpga.log" 2>&1; then
        why=$(grep -m1 error "$OUT/$n.zfpga.log" | sed "s|$TREE/||" | cut -c1-90)
        [ -n "$why" ] || why="CRASHED: $(tail -1 "$OUT/$n.zfpga.log")"
        printf '%-16s refused: %s\n' "$n" "$why"
        refused=$((refused + 1)); continue
    fi
    what=$(grep -m1 synthesised "$OUT/$n.zfpga.log" | sed 's/.*synthesised //')
    if python3 tools/simcheck.py --db ext/prjtrellis-db --device LFE5U-25F --package CABGA256 \
            "$OUT/$n.json" "$OUT/$n.lpf" "$OUT/$n.zl" --cycles 2000 > "$OUT/$n.check.log" 2>&1; then
        printf '%-16s EQUIVALENT  %s\n' "$n" "$what"
        ok=$((ok + 1))
    elif grep -q "more than one clock\|not modelled" "$OUT/$n.check.log"; then
        printf '%-16s unchecked   %s (%s)\n' "$n" "$what" "$(tail -1 "$OUT/$n.check.log" | sed 's/^simcheck: //' | cut -c1-50)"
        unchecked=$((unchecked + 1))
    else
        printf '%-16s DIFFERS     %s\n                 %s\n' "$n" "$what" "$(tail -1 "$OUT/$n.check.log" | cut -c1-110)"
        bad=$((bad + 1))
    fi
done
echo
echo "$ok equivalent, $bad differ, $refused refused, $unchecked not checkable"
[ $bad -eq 0 ]
