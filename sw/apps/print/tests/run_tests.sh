#!/bin/sh
# The print service end to end on the build machine (`make test`):
# slide's tour deck as a job, printed as PWG raster, PDF and PostScript,
# each checked by a program that is not this one -- the PWG by
# tests/pwg_decode.py (written from the standard), the PDF by qpdf and
# Poppler, the PostScript by Ghostscript, and both compared with our
# own raster (tests/compare.py). Then IPP against ippeveprinter (CUPS's
# reference IPP Everywhere printer) and a port-9100 socket. A tool that
# is not installed skips its part, and says so. docs/print.md, "Tests".

cd "$(dirname "$0")/.." || exit 1
OUT=${OUT:-/tmp/print_test}
ROOT=$(cd ../../.. && pwd)
H="/tmp/print_host --root $ROOT"
PORT=${PORT:-8631}
fail=0
skip=""

rm -rf "$OUT"
mkdir -p "$OUT"

ok() { echo "ok:   $*"; }
bad() { echo "FAIL: $*"; fail=$((fail + 1)); }
have() { command -v "$1" >/dev/null 2>&1; }

# -- jobs, as slide's /print writes them --
make -s -C ../slide job JOBOUT="$OUT/tour.zpp" >/dev/null 2>&1 || { echo "FAIL: slide job"; exit 1; }
make -s -C ../slide job JOBOUT="$OUT/nup.zpp" JOBOPTS="nup=4 cut=1 frame=1 paper=a4" >/dev/null 2>&1
make -s -C ../slide job JOBOUT="$OUT/page.zpp" JOBOPTS="paper=page" >/dev/null 2>&1
ok "jobs: $(wc -c < "$OUT/tour.zpp") bytes for the tour"

# -- a text job: the text named, no pages; print lays it out (textdoc.c) --
python3 - "$OUT" <<'PY'
import os, struct, sys
out = sys.argv[1]
text = ("Dear reader,\n\nA paragraph long enough to wrap: " + "words " * 60 + "\n"
	"  - an indented item\n\tafter a tab\nprix: 89 \u20ac, fran\u00e7ais\n\fpage two\n" +
	"".join("line %d\n" % i for i in range(80)))
open(out + "/letter.txt", "w").write(text)
def rec(op, d):
	return struct.pack("<BBHI", op, 0, 0, len(d)) + d + b"\0" * (-len(d) % 4)
job = b"ZPP1" + struct.pack("<I", 1) + rec(1, b"title=letter") + rec(1, b"text=" + (out + "/letter.txt").encode())
open(out + "/letter.zpp", "wb").write(job)
PY
if $H --pdf "$OUT/letter.pdf" "$OUT/letter.zpp" > "$OUT/letter.log"; then
	n=$(grep -o "[0-9]* sheets" "$OUT/letter.log" | cut -d" " -f1)
	if [ "$n" = "3" ]; then ok "text job: 3 pages (a form feed, then a page and a bit)"
	else bad "text job: $n pages, wanted 3"; fi
	if have qpdf; then
		qpdf --check "$OUT/letter.pdf" >/dev/null 2>&1 && ok "text job: qpdf --check" || bad "text job: qpdf --check"
	fi
else
	bad "text job"; cat "$OUT/letter.log"
fi

# -- PWG raster --
if $H --pwg "$OUT/tour.pwg" "$OUT/tour.zpp" >/dev/null &&
	python3 tests/pwg_decode.py "$OUT/tour.pwg" "$OUT" > "$OUT/pwg.log"; then
	mkdir -p "$OUT/ours"
	for f in "$OUT"/pwg-*.pgm; do mv "$f" "$OUT/ours/"; done
	ok "PWG raster: $(tail -1 "$OUT/pwg.log")"
else
	bad "PWG raster"; cat "$OUT/pwg.log"
fi
if $H --srgb --pwg "$OUT/rgb.pwg" "$OUT/tour.zpp" >/dev/null &&
	python3 tests/pwg_decode.py "$OUT/rgb.pwg" > "$OUT/rgb.log" && grep -q srgb_8 "$OUT/rgb.log"; then
	ok "PWG raster, srgb_8"
else
	bad "PWG raster, srgb_8"
fi
if $H --pwg "$OUT/nup.pwg" "$OUT/nup.zpp" >/dev/null &&
	python3 tests/pwg_decode.py "$OUT/nup.pwg" > "$OUT/nup.log" &&
	grep -q "3 pages, well formed" "$OUT/nup.log" && grep -q iso_a4 "$OUT/nup.log"; then
	ok "four to a sheet on A4, cut marks: 3 sheets for 10 cards"
else
	bad "four to a sheet"; cat "$OUT/nup.log"
fi

# -- PDF --
$H --pdf "$OUT/tour.pdf" "$OUT/tour.zpp" >/dev/null || bad "PDF written"
if have qpdf; then
	if qpdf --check "$OUT/tour.pdf" > "$OUT/qpdf.log" 2>&1; then ok "PDF: qpdf finds no errors"
	else bad "PDF: qpdf"; cat "$OUT/qpdf.log"; fi
else skip="$skip qpdf"; fi
if have pdftoppm; then
	pdftoppm -r 300 -gray -aa no -aaVector no "$OUT/tour.pdf" "$OUT/pdf"
	if python3 tests/compare.py "$OUT/ours/pwg" "$OUT/pdf" 7 > "$OUT/cmp_pdf.log"; then
		ok "PDF as Poppler draws it == our raster ($(tail -1 "$OUT/cmp_pdf.log" | cut -d' ' -f2-4))"
	else bad "PDF against our raster"; cat "$OUT/cmp_pdf.log"; fi
	$H --pdf "$OUT/page.pdf" "$OUT/page.zpp" >/dev/null
	if pdfinfo "$OUT/page.pdf" 2>/dev/null | grep -q "Page size: *720 x 48[01]"; then
		ok "PDF, paper=page: each sheet the card's own shape (10 in wide)"
	else bad "PDF, paper=page"; pdfinfo "$OUT/page.pdf" | grep "Page size"; fi
else skip="$skip pdftoppm"; fi

# -- PostScript --
$H --ps "$OUT/tour.ps" "$OUT/tour.zpp" >/dev/null || bad "PostScript written"
if have gs; then
	if gs -q -dSAFER -dBATCH -dNOPAUSE -sDEVICE=pgmraw -r300 -sOutputFile="$OUT/ps-%d.pgm" "$OUT/tour.ps" > "$OUT/gs.log" 2>&1 &&
		python3 tests/compare.py "$OUT/ours/pwg" "$OUT/ps" 7 > "$OUT/cmp_ps.log"; then
		ok "PostScript as Ghostscript draws it == our raster ($(tail -1 "$OUT/cmp_ps.log" | cut -d' ' -f2-4))"
	else bad "PostScript"; cat "$OUT/gs.log" "$OUT/cmp_ps.log"; fi
else skip="$skip gs"; fi

# -- IPP, against CUPS's reference printer --
if have ippeveprinter; then
	SPOOL="$OUT/spool"
	mkdir -p "$SPOOL"
	start() {
		$1 ippeveprinter -n 127.0.0.1 -p $PORT -k -d "$SPOOL" -M Zeitlos -m "Test printer" \
			-f image/pwg-raster,application/pdf,application/postscript "Zeitlos Test" > "$OUT/ippeve.log" 2>&1 &
		IPPPID=$!
		sleep 3
		kill -0 $IPPPID 2>/dev/null
	}
	if ! start ""; then
		# no IPv6 on this machine (a container): tests/no_ipv6.c
		cc -shared -fPIC -o /tmp/no_ipv6.so tests/no_ipv6.c -ldl 2>/dev/null
		start "env LD_PRELOAD=/tmp/no_ipv6.so" || IPPPID=""
	fi
	if [ -n "$IPPPID" ]; then
		URL=ipp://127.0.0.1:$PORT/ipp/print
		if $H --probe $URL > "$OUT/probe.log" && grep -q "pwg-raster pdf postscript" "$OUT/probe.log"; then
			ok "IPP: Get-Printer-Attributes"
		else bad "IPP probe"; cat "$OUT/probe.log"; fi
		# what is in which tray (media-col-ready): ippeveprinter has Letter
		# in main and an envelope in the by-pass tray
		if grep -q "loaded: main=21590x27940 by-pass-tray=" "$OUT/probe.log"; then
			ok "IPP: trays and what is loaded in them"
		else bad "IPP trays"; grep -A1 "trays:" "$OUT/probe.log"; fi
		for pref in auto pwg ps; do
			if $H --ipp $URL --prefer $pref "$OUT/tour.zpp" > "$OUT/ipp_$pref.log" 2>&1; then
				ok "IPP Print-Job, $pref: $(grep 'sent as' "$OUT/ipp_$pref.log" | cut -d' ' -f2-)"
			else bad "IPP Print-Job, $pref"; cat "$OUT/ipp_$pref.log"; fi
		done
		sleep 1
		n=0
		for f in "$SPOOL"/*; do
			case "$f" in
			*.pdf) qpdf --check "$f" >/dev/null 2>&1 && n=$((n + 1)) ;;
			*.pwg) python3 tests/pwg_decode.py "$f" >/dev/null && n=$((n + 1)) ;;
			*.ps) gs -q -dSAFER -dBATCH -dNODISPLAY "$f" >/dev/null 2>&1 && n=$((n + 1)) ;;
			esac
		done
		if [ $n = 3 ]; then ok "IPP: the printer kept a PDF, a PWG raster and a PostScript file, all sound"
		else bad "IPP: $n of 3 spooled documents sound"; ls -la "$SPOOL"; fi
		kill $IPPPID 2>/dev/null
	else
		skip="$skip ippeveprinter(would-not-start)"
	fi
else skip="$skip ippeveprinter"; fi

# -- a port-9100 socket printer --
python3 - "$OUT/socket.ps" <<'EOF' &
import socket, sys
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('127.0.0.1', 9100)); s.listen(1); s.settimeout(20)
c, _ = s.accept(); f = open(sys.argv[1], 'wb')
while True:
    d = c.recv(65536)
    if not d: break
    f.write(d)
EOF
LPID=$!
sleep 1
if $H --socket socket://127.0.0.1:9100 "$OUT/tour.zpp" >/dev/null; then
	wait $LPID
	if cmp -s "$OUT/socket.ps" "$OUT/tour.ps"; then ok "socket 9100: the PostScript, byte for byte"
	else bad "socket 9100: not the PostScript file"; fi
else
	bad "socket 9100"; kill $LPID 2>/dev/null
fi

[ -n "$skip" ] && echo "skipped:$skip"
if [ $fail = 0 ]; then echo "print end to end: all passed"; else echo "print end to end: $fail failed"; fi
exit $fail
