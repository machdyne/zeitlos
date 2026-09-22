"""
Zeitlos
Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.

Recipe parsing for tools/speech.

The format is deliberately the same shape as tools/ask's specs (see
docs/ask_app.md): `key = value` lines, plus indented blocks that
describe one source each. It is read by people as often as by the
tool, because it is also where every input's LICENCE and CHECKSUM are
written down -- see docs/tts_data.md, "Provenance".

    source moby
        url      https://www.gutenberg.org/...
        mirror   https://codeload.github.com/...
        kind     zip
        mirror_kind tar.gz
        take     moby-project-master/moby/mpron/mobypron.unc -> mpron.txt
        refuse   moby-project-master/moby/mpron/cmudict0.3
        sha256   mpron.txt eab1c6df...
        licence  public domain -- ...

Rules the parser enforces, because getting them wrong is how
non-public-domain data would end up in a build:

  - every source must state a `licence`;
  - a `refuse` path is never extracted, whatever else asks for it.
"""

import os


class SpecError(Exception):
    pass


class Source:

    def __init__(self, name):
        self.name = name
        self.url = None
        self.mirror = None
        self.mirror_parts = []      # a mirror published in pieces
        self.mirror_kind = None     # when the mirror packages it differently
        self.kind = None            # tar.gz | tar.bz2 | zip | file | books
        self.takes = []             # (member, saved-as)
        self.refuses = []
        self.sums = {}              # saved-as -> sha256
        self.licence = None
        self.note = None
        self.ids = []               # books: Project Gutenberg ebook ids

    def check(self):
        if not self.licence:
            raise SpecError("source %s: no licence stated" % self.name)
        if not self.url and not self.ids:
            raise SpecError("source %s: no url" % self.name)
        if self.kind not in ("tar.gz", "tar.bz2", "zip", "file", "books"):
            raise SpecError("source %s: unknown kind %r" % (self.name, self.kind))


class Spec:

    def __init__(self, path):
        self.path = path
        self.settings = {}
        self.sources = []
        self._parse(path)

    def source(self, name):
        for s in self.sources:
            if s.name == name:
                return s
        return None

    def _parse(self, path):

        cur = None

        for lineno, raw in enumerate(open(path), 1):

            line = raw.split("#", 1)[0].rstrip()
            if not line.strip():
                continue

            indented = raw[0] in " \t"
            parts = line.split()

            if not indented:
                cur = None
                if parts[0] == "source":
                    if len(parts) != 2:
                        raise SpecError("%s:%d: source needs a name" % (path, lineno))
                    cur = Source(parts[1])
                    self.sources.append(cur)
                elif parts[0] == "include":
                    other = os.path.join(os.path.dirname(path), parts[1] + ".spec")
                    inc = Spec(other)
                    self.settings.update(inc.settings)
                    self.sources.extend(inc.sources)
                elif "=" in line:
                    k, v = line.split("=", 1)
                    self.settings[k.strip()] = v.strip()
                else:
                    raise SpecError("%s:%d: not understood: %s" % (path, lineno, line))
                continue

            if cur is None:
                raise SpecError("%s:%d: indented line outside a source" % (path, lineno))

            key, rest = parts[0], line.strip()[len(parts[0]):].strip()

            if key == "url":
                cur.url = rest
            elif key == "mirror":
                cur.mirror = rest
            elif key == "mirror_kind":
                cur.mirror_kind = rest
            elif key == "mirror_part":
                cur.mirror_parts.append(rest)
            elif key == "kind":
                cur.kind = rest
            elif key == "take":
                if "->" not in rest:
                    raise SpecError("%s:%d: take needs 'member -> name'" % (path, lineno))
                member, name = [x.strip() for x in rest.split("->", 1)]
                cur.takes.append((member, name))
            elif key == "refuse":
                cur.refuses.append(rest.split()[0])
            elif key == "sha256":
                name, digest = rest.split()
                cur.sums[name] = digest
            elif key == "licence" or key == "license":
                cur.licence = rest
            elif key == "note":
                cur.note = (cur.note + " " if cur.note else "") + rest
            elif key == "ids":
                cur.ids.extend(int(x) for x in rest.replace(",", " ").split())
            else:
                raise SpecError("%s:%d: unknown key %r" % (path, lineno, key))

        for s in self.sources:
            s.check()
