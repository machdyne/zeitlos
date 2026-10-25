#!/usr/bin/env python3
"""
The function keys mean the same thing in every app (docs/keys.md):

    F1 help   F2 rename/edit   F3 find/next   F4 the app's prompt
    F5-F8 the app's own   F9 reserved   F10 colour (screen apps, the
    arcade runtime's)   F11 full screen / game mode
    F12 app home (term, web, files) / reserved

The meanings have names in sw/common/zkbd.h (Z_KEY_HELP, Z_KEY_RENAME,
Z_KEY_FIND, Z_KEY_PROMPT, Z_KEY_COLOUR, Z_KEY_FULL_SCREEN,
Z_KEY_APP_HOME), and apps
use those, so the source says what a key does. This fails the build
when:

  - an app binds F1-F4, F10, F11 or F12 by number (Z_KEY_F1 ...)
    instead of by meaning;
  - anything binds F9 (reserved);
  - Z_KEY_COLOUR is used outside the arcade runtime (zarcade_sys.c),
    the dual apps (view, poker, chess, the casino tables) and wm (its
    Super+F10, desktop colour): the colour key is theirs, the same in
    every one;
  - Z_KEY_APP_HOME is used outside the apps with a home (term, web,
    files);
  - a help text pairs a key with "full screen" other than F11, lists
    F9, or lists F10 as anything but colour.

Exempt: the keyboard layer (zkbd.c), term (it forwards F1-F10 to the
program inside, as VT sequences), the REPL's Scheme windows (they pass
every key on to the program) and hello_win (a key tester), plus tests.

Run by `make -C sw/apps` beside check_subjects.py; also standalone:

    python3 tools/check_fkeys.py [sw]
"""

import os
import re
import sys

root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.path.dirname(os.path.abspath(__file__)), '..', 'sw')

EXEMPT = ('common/zkbd.c', 'apps/term/term.c', 'common/zapi/zapi.c',
          'apps/hello_win/hello_win.c')
HOME_OK = ('apps/term/', 'apps/web/', 'apps/files/')
# the runtime, and the dual apps (docs/app_kinds.md): F10 is their
# switch to the colour screen
COLOUR_OK = ('common/games/zarcade_sys.c', 'apps/view/', 'apps/poker/',
             'apps/chess/', 'apps/blackjack/', 'apps/craps/', 'apps/roulette/',
             'apps/slots/', 'apps/wm/')

NUMBERED = re.compile(r'\bZ_KEY_F(1|2|3|4|9|10|11|12)\b')
HOME = re.compile(r'\bZ_KEY_APP_HOME\b')
COLOUR = re.compile(r'\bZ_KEY_COLOUR\b')
# "F2\tFull screen" in a help box, "F6 FULL SCREEN" on a title screen
FULL = re.compile(r'\bF(\d{1,2})(?:\\t|\s+)full[ -]?screen', re.IGNORECASE)
RESERVED_HELP = re.compile(r'"[^"\n]*?\bF(9|10)(?:\\t|\s{2,})(?!colou?r)', re.IGNORECASE)

problems = []
for d, _, files in os.walk(root):
    rel_d = os.path.relpath(d, root).replace('\\', '/')
    if '/ext' in '/' + rel_d or '/tests' in '/' + rel_d or rel_d.startswith('ext'):
        continue
    for f in files:
        if not f.endswith(('.c', '.h')):
            continue
        rel = (rel_d + '/' + f) if rel_d != '.' else f
        if rel in EXEMPT or rel == 'common/zkbd.h':
            continue
        if not (rel.startswith('apps/') or rel.startswith('common/')):
            continue
        with open(os.path.join(d, f), errors='replace') as fh:
            lines = fh.read().split('\n')
        for n, line in enumerate(lines, 1):
            code = line.split('//')[0]
            m = NUMBERED.search(code)
            if m:
                k = m.group(1)
                why = ('reserved' if k == '9' else
                       'use its meaning name from zkbd.h')
                problems.append('%s:%d: Z_KEY_F%s -- %s' % (rel, n, k, why))
            if COLOUR.search(code) and not rel.startswith(COLOUR_OK):
                problems.append('%s:%d: Z_KEY_COLOUR outside the arcade '
                                'runtime -- F10 is its colour key' % (rel, n))
            if HOME.search(code) and not rel.startswith(HOME_OK):
                problems.append('%s:%d: Z_KEY_APP_HOME outside term, web, '
                                'files -- F12 is reserved here' % (rel, n))
            for m in FULL.finditer(line):
                if m.group(1) != '11':
                    problems.append('%s:%d: help pairs F%s with full screen '
                                    '(F11 is full screen)' % (rel, n, m.group(1)))
            for m in RESERVED_HELP.finditer(line):
                problems.append('%s:%d: help lists F%s, which is %s'
                                % (rel, n, m.group(1), 'reserved' if m.group(1) == '9'
                                   else 'colour only'))

if problems:
    print('check_fkeys: function keys that break docs/keys.md:')
    for p in problems:
        print('  ' + p)
    sys.exit(1)
print('check_fkeys: every app follows docs/keys.md')
