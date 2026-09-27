#!/usr/bin/env python3
"""
Every message subject used in sw/ must be defined through a block in
sw/common/zsubjects.h (docs/messaging.md, "Subjects and tags").

zsubjects.h makes the REGISTERED protocols unable to collide: their
blocks are checked disjoint and each header checks its subjects stay in
its block, at compile time. What the compiler cannot see is a protocol
that never registered -- a new header with `#define Z_FOO_HELLO 150` --
and that is exactly how wm, the port protocol, the REPL protocol, net
and ask came to share numbers. This closes that gap: it finds every
name the tree sends as a subject, receives by subject, or waits for,
and fails if one is not written as (Z_SUBJ_<BLOCK> + n).

Run by `make -C sw/apps` before anything is compiled; also standalone:

    python3 tools/check_subjects.py [sw]
"""

import os
import re
import sys

root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.path.dirname(os.path.abspath(__file__)), '..', 'sw')

src = {}
for d, _, files in os.walk(root):
    if '/ext' in d.replace('\\', '/'):
        continue
    for f in files:
        if f.endswith(('.c', '.h')):
            p = os.path.join(d, f)
            with open(p, errors='replace') as fh:
                src[p] = fh.read()

# Where a subject appears: the second argument of a send, a comparison
# with a message's subject, or the subject a wait is for.
USE = [
    re.compile(r'\b(?:z_msg_new_send|z_msg_send|k_msg_send|k_msg_new_send|'
               r'z_msg_reply|k_msg_post)\s*\(\s*[^,;()]*(?:\([^()]*\))?[^,;()]*,'
               r'\s*([A-Z][A-Z0-9_]+)\b'),
    re.compile(r'\bsubject\s*[!=]=\s*([A-Z][A-Z0-9_]+)\b'),
    re.compile(r'\bz_msg_wait\s*\([^,;]*,\s*([A-Z][A-Z0-9_]+)\b'),
]

used = {}
for p, s in src.items():
    for rx in USE:
        for m in rx.finditer(s):
            used.setdefault(m.group(1), p)

defs = {}
DEF = re.compile(r'^\s*#define\s+([A-Z][A-Z0-9_]+)\s+(.+?)\s*(?://.*|/\*.*)?$', re.M)
for p, s in src.items():
    for m in DEF.finditer(s):
        defs.setdefault(m.group(1), (m.group(2), p))

bad = []
for name, where in sorted(used.items()):
    if name not in defs:
        continue            # a variable or a parameter, not a constant
    body, path = defs[name]
    if not re.search(r'\bZ_SUBJ_[A-Z0-9_]+\b', body):
        bad.append((name, body, os.path.relpath(path, root), os.path.relpath(where, root)))

if bad:
    print('check_subjects: message subjects not from a block in '
          'sw/common/zsubjects.h:', file=sys.stderr)
    for name, body, path, where in bad:
        print(f'  {name} = {body}  ({path}; used in {where})', file=sys.stderr)
    print('Give the protocol a block in zsubjects.h and define each subject '
          'as (Z_SUBJ_<BLOCK> + n) -- see that file.', file=sys.stderr)
    sys.exit(1)

print(f'check_subjects: {len(used)} subject names, all from zsubjects.h blocks')
