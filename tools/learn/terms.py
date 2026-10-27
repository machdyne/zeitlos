#!/usr/bin/env python3
#
# Zeitlos
# Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
#
# terms.py -- is every term taught before a lesson uses it?
# (docs/learn_authoring.md, "Rules from the first courses")
#
#   tools/learn/terms.py [--list] FOLDER-OF-COURSES-OR-COURSE...
#
# A lesson teaches the terms in its `(keywords ...)`. A lesson that uses
# one of those terms must come after a lesson that teaches it on the
# learner's path: earlier in the same course, or in a course this one
# `needs` (course.cfg, followed through). A term taught only in a course
# that comes later on the path (one that needs this course) is flagged
# too. Courses that have nothing to do with each other are not compared:
# the fact-check judges whether a word is known there. A lesson may also teach a term
# itself -- put it in its own keywords and define it, in bold, in the
# step that first uses it. A term that is also an everyday word is
# written with a note in brackets, `(keywords "word (of bits)")`, and is
# then checked only in the courses that teach it.
#
# The first course on hardware said "a voltage on the wire" to learners
# who had never met the word. This finds that mistake whenever the word
# is taught somewhere in the pack; a word taught nowhere it cannot know
# about, and the fact-check has to catch (it is asked to).
#
# Problems are printed as `file:line: warning: ...`; the exit status is
# 1 if there were any.

import os
import re
import sys

NUM = re.compile(r'^\d+-')


def numbered(d):
    return sorted(e for e in os.listdir(d) if NUM.match(e))


def lessons_of(course):
    """The course's lesson files, in course order."""
    out = []

    def walk(d):
        for e in numbered(d):
            p = os.path.join(d, e)
            if os.path.isdir(p):
                walk(p)
            elif e.endswith('.md'):
                out.append(p)
    walk(course)
    return out


def read_cfg(course):
    cfg = {'needs': []}
    try:
        for line in open(os.path.join(course, 'course.cfg'), encoding='utf-8'):
            line = line.split('#', 1)[0].strip()
            if '=' not in line:
                continue
            k, v = (x.strip() for x in line.split('=', 1))
            if k == 'needs':
                cfg['needs'].append(v)
            else:
                cfg[k] = v
    except OSError:
        pass
    return cfg


STRING = re.compile(r'"((?:[^"\\]|\\.)*)"')
# blocks whose words a learner reads (their strings), and blocks that are
# not words at all
DATA_READ = {'check', 'example', 'note', 'warn', 'say', 'try', 'cards', 'card'}


def parse(path):
    """(keywords, steps) where steps is a list of [(line, text)]."""
    keywords = []
    steps = [[]]
    fence = None
    block = []
    for n, line in enumerate(open(path, encoding='utf-8'), 1):
        s = line.rstrip('\n')
        if fence is not None:
            if s.startswith('```'):
                body = '\n'.join(t for _, t in block)
                if fence == 'lesson':
                    m = re.search(r'\(keywords((?:\s+"[^"]*")+)\s*\)', body)
                    if m:
                        keywords += STRING.findall(m.group(1))
                elif fence in DATA_READ:
                    for ln, t in block:
                        for x in STRING.findall(t):
                            steps[-1].append((ln, x))
                fence = None
                block = []
            else:
                block.append((n, s))
            continue
        if s.startswith('```'):
            fence = s[3:].strip().split(' ')[0]
            continue
        if s.strip() == '---':
            steps.append([])
            continue
        if s.startswith('#'):
            continue		# a title names what is coming; it is not a use
        # links: the words, not the target
        s = re.sub(r'\]\([^)]*\)', ']', s)
        steps[-1].append((n, s))
    return [k.strip() for k in keywords if k.strip()], steps


def pattern(term):
    """A term and its plural; an acronym (FRAM, IP) only in capitals."""
    t = re.escape(term).replace(r'\ ', r'[\s-]+')
    plural = '(?:es)?' if re.search(r'(s|x|z|ch|sh)$', term, re.I) else 's?'
    acronym = any(c.isupper() for c in term) and not any(c.islower() for c in term)
    return re.compile(r'(?<![\w-])' + t + plural + r'(?![\w-])', 0 if acronym else re.I)


def main(argv):
    listing = '--list' in argv
    args = [a for a in argv if not a.startswith('--')]
    if not args:
        print('usage: terms.py [--list] FOLDER-OF-COURSES-OR-COURSE...', file=sys.stderr)
        return 2
    courses = {}
    for a in args:
        a = a.rstrip('/')
        if os.path.exists(os.path.join(a, 'course.cfg')):
            courses[os.path.basename(a)] = a
        else:
            for e in sorted(os.listdir(a)):
                p = os.path.join(a, e)
                if os.path.exists(os.path.join(p, 'course.cfg')):
                    courses[e] = p
    cfgs = {c: read_cfg(p) for c, p in courses.items()}

    def needed(c, seen=None):
        seen = set() if seen is None else seen
        for n in cfgs.get(c, {}).get('needs', []):
            if n not in seen:
                seen.add(n)
                needed(n, seen)
        return seen

    # who teaches what: term -> [(course, index, path)]
    lessons = {}
    taught = {}
    local = set()
    spelled = {}
    for c, p in courses.items():
        lessons[c] = []
        for i, f in enumerate(lessons_of(p)):
            kw, steps = parse(f)
            lessons[c].append((f, kw, steps))
            for k in kw:
                # "word (of bits)": also an everyday word, so its uses are
                # only checked in the courses that teach it
                orig = k.split(' (')[0].strip()
                t = orig.lower()
                spelled.setdefault(t, orig)
                if '(' in k:
                    local.add(t)
                taught.setdefault(t, []).append((c, i, f))
    if listing:
        for t in sorted(taught):
            print(t + ': ' + ', '.join(os.path.relpath(f) for _, _, f in taught[t]))
        return 0
    pats = {t: pattern(spelled[t]) for t in taught}
    # a longer term that holds this one ("binary digit" holds "binary") is
    # its own term: its uses are not uses of the shorter one
    longer = {t: [pats[u] for u in taught if u != t and pats[t].search(spelled[u])] for t in taught}
    # the courses after this one on a learner's path: those that need it
    after = {c: {d for d in courses if c in needed(d)} for c in courses}
    problems = 0
    for c in courses:
        before = needed(c)
        path = {c} | after[c]
        for i, (f, kw, steps) in enumerate(lessons[c]):
            own = {k.lower().split(' (')[0].strip() for k in kw}
            flat = ' '.join(t for step in steps for _, t in step).lower()
            for t, pat in pats.items():
                # only a term taught on this course's path: in it, or in a
                # course that comes after it (a course nobody here needs
                # is the fact-check's to judge)
                if not any(tc in path for tc, _, _ in taught[t]):
                    continue
                # an everyday word, "row (of a sheet)": only where it is taught
                if t in local and not any(tc == c for tc, _, _ in taught[t]):
                    continue
                if t.split()[0][:4] not in flat:
                    continue
                first = None
                for si, step in enumerate(steps):
                    for ln, text in step:
                        for lp in longer[t]:
                            text = lp.sub(' ', text)
                        if pat.search(text):
                            first = (si, ln)
                            break
                    if first:
                        break
                if not first:
                    continue
                if t in own:
                    # taught here: defined, in bold, in the step that first uses it
                    bold = re.compile(r'\*\*[^*]*' + pat.pattern + r'[^*]*\*\*', pat.flags)
                    if not any(bold.search(text) for _, text in steps[first[0]]):
                        print('%s:%d: warning: "%s" is used here before this lesson defines it '
                              '(define it, in bold, in the step that first uses it)' % (f, first[1], spelled[t]))
                        problems += 1
                    continue
                ok = any((tc == c and ti < i) or tc in before for tc, ti, _ in taught[t])
                if not ok:
                    where = taught[t][0]
                    why = ('a later lesson' if where[0] == c else
                           'the course %s, which comes after this one' % where[0])
                    print('%s:%d: warning: "%s" is used before it is taught (%s, in %s); '
                          'teach it first, or define it here (and list it in keywords)' %
                          (f, first[1], spelled[t], os.path.relpath(where[2]), why))
                    problems += 1
    n = sum(len(v) for v in lessons.values())
    print('terms: %d course%s, %d lessons, %d terms: %d warning%s' %
          (len(courses), '' if len(courses) == 1 else 's', n, len(taught), problems,
           '' if problems == 1 else 's'))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
