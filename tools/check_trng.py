#!/usr/bin/env python3
#
# Zeitlos
# Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
#
# Verifies that rtl/trng.v's ring oscillators survived the toolchain.
#
# -- Why this exists, and why it checks LENGTH and not just count --
#
# A ring oscillator IS a combinational loop, so both yosys and nextpnr
# report every one of them. Those messages are the design working. They
# are also indistinguishable, in a build log, from a real accidental
# loop -- and a warning nobody can act on is one everybody learns to
# scroll past, including on the day it means something.
#
# The first version of this script counted loops in the yosys log. IT
# WOULD HAVE PASSED A COMPLETELY BROKEN BUILD, and that is the whole
# reason this rewrite exists. What actually happened on a real ECP5
# build:
#
#   yosys:    8 loops reported, on intermediate chain wires. Looks fine.
#   nextpnr:  8 loops reported, EACH ONE CELL LONG, on ro_net[0], [27],
#             [54], [81] ... -- the chain heads.
#
# One cell means the whole inverter chain had been folded: thirteen
# inversions is algebraically one, so `ro_net[0] = enable & ~ro_net[12]`
# became `ro_net[0] = enable & ro_net[0]`. That is a latch that settles
# to zero, not an oscillator. The loop COUNT was still correct and still
# eight. Only the LENGTH gave it away.
#
# So this parses nextpnr's loop report, which lists every cell in each
# cycle, and checks that each loop is as long as the oscillator it is
# supposed to be. Oscillator gi has RO_BASE + 2*gi stages, so with the
# defaults the eight loops must have 13, 15, 17, 19, 21, 23, 25 and 27
# cells.
#
# What this does NOT prove: that the oscillators oscillate on silicon.
# Place-and-route runs after synthesis and physics runs after that. The
# runtime health monitor in rtl/trng.v covers those. See docs/trng.md
# for the full verification order -- this script is one step of four,
# and it is the step that catches the failure that is otherwise
# completely silent.
#
# -- Usage --
#
#     make BOARD=... 2>&1 | tee build.log
#     python3 tools/check_trng.py build.log
#
# Exits 0 on success, 1 on a bad bank, 2 if it cannot tell. The last
# case is deliberately not a pass: "I could not check" and "I checked
# and it was fine" must never look the same to a build.

import argparse
import re
import sys

# nextpnr prints, per loop:
#     Info:     loop 1:
#     Info:         trng_i.ro_net_LUT4_Z_10.C (trng_i.ro_net[135])
#     Info:         trng_i.ro_net_LUT4_Z_10.F (trng_i.ro_net[135])
# i.e. two lines per cell (an input port and an output port), so a
# 13-stage ring produces 26 lines. Counting DISTINCT CELL NAMES rather
# than lines avoids depending on that being exactly two.
NPNR_HEADER = re.compile(r'Found\s+(\d+)\s+combinational loops')
NPNR_LOOP = re.compile(r'^\s*(?:Info:)?\s*loop\s+(\d+):', re.I)
NPNR_CELL = re.compile(r'^\s*(?:Info:)?\s+(\S+)\.\w+\s*\(([^)]*)\)')

YOSYS_LOOP = re.compile(r'found logic loop in module')
TRNG_HINT = re.compile(r'trng|ro_net', re.I)


def expected_lengths(num_ro, ro_base):
    return [ro_base + 2 * i for i in range(num_ro)]


def parse_nextpnr(lines):
    """Return a list of loop lengths (in cells) for TRNG loops, or None
    if this log has no nextpnr loop report at all."""

    saw_report = False
    loops = []
    current = None

    for line in lines:

        if NPNR_HEADER.search(line):
            saw_report = True
            continue

        if NPNR_LOOP.match(line):
            if current is not None:
                loops.append(current)
            current = {'cells': [], 'nets': []}
            continue

        if current is not None:
            m = NPNR_CELL.match(line)
            if m:
                current['cells'].append(m.group(1))
                current['nets'].append(m.group(2))
                continue
            # A non-blank line that is not a cell entry ends this loop
            # block. Blank lines are tolerated so a little formatting
            # variation between nextpnr versions doesn't break parsing.
            if line.strip():
                loops.append(current)
                current = None

    if current is not None:
        loops.append(current)

    if not saw_report and not loops:
        return None

    out = []
    for lp in loops:
        blob = ' '.join(lp['cells']) + ' ' + ' '.join(lp['nets'])
        if TRNG_HINT.search(blob):
            out.append(len(set(lp['cells'])))

    return out


def parse_yosys(text):
    """Count yosys-reported loops mentioning the TRNG. Weaker than the
    nextpnr check -- see this file's header -- and used only as a
    fallback when no nextpnr report is present."""

    blocks = YOSYS_LOOP.split(text)
    if len(blocks) < 2:
        return None
    return sum(1 for b in blocks[1:]
               if TRNG_HINT.search('\n'.join(b.splitlines()[:12])))


def fail(msg_lines):
    print('', file=sys.stderr)
    for line in msg_lines:
        print(line, file=sys.stderr)
    print('', file=sys.stderr)
    print('  Do not flash this bitstream for anything that needs a key.',
          file=sys.stderr)
    print('  See docs/trng.md, "The risk that matters".', file=sys.stderr)
    print('', file=sys.stderr)
    return 1


def main():

    ap = argparse.ArgumentParser(
        description='Verify the TRNG ring oscillators survived the toolchain.')
    ap.add_argument('logfile', help='combined yosys + nextpnr build log')
    ap.add_argument('--num-ro', type=int, default=8,
                    help='NUM_RO in rtl/trng.v (default 8)')
    ap.add_argument('--ro-base', type=int, default=13,
                    help='RO_BASE in rtl/trng.v (default 13)')
    args = ap.parse_args()

    try:
        with open(args.logfile, 'r', errors='replace') as f:
            text = f.read()
    except OSError as e:
        print('check_trng: cannot read %s: %s' % (args.logfile, e),
              file=sys.stderr)
        return 2

    lines = text.splitlines()
    want = expected_lengths(args.num_ro, args.ro_base)

    lengths = parse_nextpnr(lines)

    if lengths is not None:

        if len(lengths) != args.num_ro:
            return fail([
                'check_trng: FAIL -- nextpnr reported %d TRNG loops, '
                'expected %d.' % (len(lengths), args.num_ro),
                '',
                '  Some ring oscillators were optimised away. The block will',
                '  still build and still return 32-bit words. They will not',
                '  be random.'])

        if sorted(lengths) != sorted(want):
            return fail([
                'check_trng: FAIL -- TRNG loops are the wrong length.',
                '',
                '    found:    %s' % sorted(lengths),
                '    expected: %s' % sorted(want),
                '',
                '  Short loops mean the inverter chains were folded (thirteen',
                '  inversions is algebraically one), leaving a latch rather',
                '  than an oscillator. This is exactly the failure found on',
                '  ECP5 with plain `assign x = ~y` chains, and is why',
                '  rtl/trng.v instantiates LUT primitives instead.',
                '',
                '  If you built with -DTRNG_RO_GENERIC, or on a vendor',
                '  rtl/trng.v has no primitive branch for, that is the cause:',
                '  add a branch for your LUT primitive.'])

        print('check_trng: ok -- %d ring oscillators intact, lengths %s'
              % (len(lengths), sorted(lengths)))
        return 0

    # No nextpnr report in this log. Fall back to the yosys count, but
    # say plainly that it proves much less.
    count = parse_yosys(text)

    if count is None:
        print('check_trng: cannot verify -- no loop report from either yosys '
              'or nextpnr in %s' % args.logfile, file=sys.stderr)
        print('  Was the log captured with 2>&1? Is `TRNG enabled in '
              'rtl/boards.vh?', file=sys.stderr)
        return 2

    if count != args.num_ro:
        return fail([
            'check_trng: FAIL -- yosys reported %d TRNG loops, expected %d.'
            % (count, args.num_ro)])

    print('check_trng: PARTIAL -- yosys reports %d loops, which is the right '
          'number,' % count)
    print('  but this log has no nextpnr report, and the yosys count alone')
    print('  cannot detect a folded chain. Capture the full build log and')
    print('  re-run before trusting this bank.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
