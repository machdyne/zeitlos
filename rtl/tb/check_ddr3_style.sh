#!/bin/bash
# Project rules, checked rather than remembered:
#   * plain Verilog, not SystemVerilog
#   * no local declarations: nothing declared inside a generate,
#     begin/end, task or function body -- declarations live at module
#     scope, one tab in
cd "$(dirname "$0")/../.."
fail=0
SV='\b(logic|always_ff|always_comb|always_latch|typedef|interface|unique|priority)\b'
for f in rtl/mem/ddr3*.v rtl/tb/ddr3_*.v rtl/tb/tb_ddr3*.v; do
	[ -f "$f" ] || continue
	hits=$(grep -nP '^\t{2,}(wire|reg|integer|localparam|genvar)\b' "$f")
	if [ -n "$hits" ]; then
		echo "LOCAL DECLARATION in $f:"; echo "$hits" | head -5; fail=1
	fi
	# Strip comments first: English words like "logic" are fine there.
	sv=$(sed -e 's|//.*||' -e '/^[[:space:]]*\*/d' -e '/^[[:space:]]*\/\*/d' "$f" \
		| grep -nE "$SV")
	if [ -n "$sv" ]; then
		echo "SYSTEMVERILOG in $f:"; echo "$sv" | head -3; fail=1
	fi
done
[ $fail = 0 ] && echo "style: ok"
exit $fail
