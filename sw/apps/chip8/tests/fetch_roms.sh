#!/bin/sh
#
# chip8 -- fetch the CHIP-8 test suite ROMs into tests/roms/.
#
# The suite is Timendus' chip8-test-suite, which is MIT licensed. The
# ROMs are deliberately NOT committed to this tree:
#
#   - They are somebody else's project with its own release cadence,
#     and a vendored copy goes stale silently.
#   - Nothing in the normal build needs them. tests/test_core.c is the
#     part that must always run, and it needs no external anything;
#     the suite is an additional check that runs when it can.
#
# tests/suite.sh exits 77 ("skipped") rather than failing when they are
# absent, so a machine with no network still passes.
#
# Usage: ./tests/fetch_roms.sh

set -e

DIR="$(dirname "$0")/roms"
BASE="https://raw.githubusercontent.com/Timendus/chip8-test-suite/main/bin"

ROMS="1-chip8-logo 2-ibm-logo 3-corax+ 4-flags 5-quirks 6-keypad 7-beep 8-scrolling"

mkdir -p "$DIR"

if command -v curl > /dev/null 2>&1; then
	GET="curl -sfL -o"
elif command -v wget > /dev/null 2>&1; then
	GET="wget -q -O"
else
	echo "fetch_roms.sh: need curl or wget" >&2
	exit 1
fi

fail=0
for r in $ROMS; do
	if $GET "$DIR/$r.ch8" "$BASE/$r.ch8"; then
		echo "got $r.ch8"
	else
		echo "MISS $r.ch8"
		fail=1
	fi
done

echo
echo "ROMs in $DIR"
[ "$fail" = "1" ] && echo "(some downloads failed -- suite.sh will report those as MISS)"

exit 0
