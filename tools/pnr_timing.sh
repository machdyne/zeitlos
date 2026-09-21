#!/bin/sh
#
# Zeitlos
# Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
#
# Print nextpnr's routed timing result from a pnr.log.
#
# nextpnr writes "Max frequency for clock" twice: an estimate after
# placement, then the real result after routing. The estimate often
# fails on a design the router then closes. Grepping the whole log for
# "FAIL at" reports that estimate.
#
# The last line of each clock is the routed result. ro_clk[*] are the
# TRNG ring oscillators (rtl/trng.v): each clocks one divide-by-two
# flop, nextpnr reports them as domains, and they are not constraints,
# so they are dropped here the same way `make path` is not asked to
# care about them.
#
# A clock that still fails after routing prints the warning, and the
# critical path of THAT clock from the routed report. The path is
# only the routed one: reports that sit above the last frequency line
# belong to the placement estimate.
#
#     tools/pnr_timing.sh output/<board>/pnr.log <board>
#
# Exit 0 either way. A missed clock still programs; the warning is how
# you find out. Missing log is the one error.

if [ $# -lt 1 ] || [ ! -f "$1" ]; then
	echo "no ${1:-pnr.log} -- build first" >&2
	exit 1
fi

awk -v board="${2:-}" '
function qname(s,    a, b) {
	a = index(s, "'\''")
	if (!a) return ""
	b = index(substr(s, a + 1), "'\''")
	if (!b) return ""
	return substr(s, a + 1, b - 1)
}

function emit(r,    i, k, parts) {
	k = split(rep[r], parts, "\n")
	for (i = 1; i <= k && shown < 30; i++) {
		if (parts[i] == "") continue
		print parts[i]
		shown++
	}
}

/Max frequency for clock/ {
	lastfreq = NR
	line = $0
	sub(/^Info: /, "", line)
	if (index(line, "ro_clk")) next
	name = qname(line)
	if (name == "") next
	if (!(name in seen)) {
		seen[name] = 1
		seq[++n] = name
	}
	freq[name] = line
	bad[name] = index(line, "FAIL at") ? 1 : 0
	next
}

/Critical path report for clock/ {
	rn++
	rep_at[rn] = NR
	rep_name[rn] = qname($0)
	inrep = 1
	next
}

inrep && NF == 0 { inrep = 0; next }

inrep && (/Source/ || /Sink/ || /\.v:[0-9]/) {
	rep[rn] = rep[rn] $0 "\n"
}

END {
	any = 0
	for (i = 1; i <= n; i++) {
		print freq[seq[i]]
		if (bad[seq[i]]) any = 1
	}
	if (!any) exit 0

	print ""
	print "*** TIMING NOT MET -- the bitstream will program and"
	print "*** misbehave intermittently. Critical path:"
	print ""

	# Routed reports sit below the last frequency line. If this nextpnr
	# only printed paths for the placement estimate, fall back to those
	# so a real failure still shows a path instead of an empty warning.
	shown = 0
	for (i = 1; i <= n && shown < 30; i++) {
		if (!bad[seq[i]]) continue
		for (r = 1; r <= rn && shown < 30; r++) {
			if (rep_name[r] != seq[i]) continue
			if (rep_at[r] <= lastfreq) continue
			emit(r)
		}
	}
	if (!shown) {
		for (i = 1; i <= n && shown < 30; i++) {
			if (!bad[seq[i]]) continue
			for (r = 1; r <= rn && shown < 30; r++) {
				if (rep_name[r] != seq[i]) continue
				emit(r)
			}
		}
	}

	print ""
	if (board != "")
		print "*** full detail: make path BOARD=" board
	else
		print "*** full detail: make path"
	print ""
}
' "$1"
