#
# Zeitlos ask -- distribution recipes.
#
# The format is release/hw/boards/*.spec's format, deliberately: `key =
# value`, `#` comments, blank lines ignored, and a key may repeat to
# build a list. Nobody should have to learn a second configuration
# syntax to add a dataset, and this tree already has one that works and
# that people have read.
#
# A recipe is the ONLY place that says what is in a distribution. It
# names sources, where each one comes from, and what version of it was
# used. Everything downstream -- the card tree, the index, the
# manifest, the eval report -- is derived from it, so two builds from
# the same recipe and the same inputs produce the same bytes.
#
# WHY NOT YAML: one more dependency for a format this tree does not
# otherwise use, on a tool that has to run on whatever machine someone
# is training on. The recipes are twenty lines of key/value.
#

import os
import re


class SpecError(Exception):
    pass


_KV = re.compile(r"^\s*([A-Za-z0-9_.\-]+)\s*=\s*(.*?)\s*$")

# Keys that accumulate rather than replace. Everything else is
# last-one-wins, which matches how release/lib/spec.py behaves and is
# what somebody editing a copy of a recipe expects.
LIST_KEYS = ("source", "include", "repo", "fetch", "images")

# Datasets whose text is model-written rather than human-written.
# Comma-separated dataset names; see pack.py's DS_GENERATED.


class Spec:
    """One parsed recipe. `get` for scalars, `getlist` for lists."""

    def __init__(self, path):
        self.path = path
        self.name = os.path.splitext(os.path.basename(path))[0]
        self.values = {}
        self.lists = {k: [] for k in LIST_KEYS}
        self._parse(path)

    def _parse(self, path):
        if not os.path.exists(path):
            raise SpecError("no such recipe: %s" % path)
        for lineno, raw in enumerate(open(path, encoding="utf-8"), 1):
            line = raw.split("#", 1)[0]
            if not line.strip():
                continue
            m = _KV.match(line)
            if not m:
                raise SpecError("%s:%d: not `key = value`: %s"
                                % (path, lineno, raw.rstrip()))
            key, val = m.group(1), m.group(2)
            if key in LIST_KEYS:
                self.lists[key].append(val)
            else:
                self.values[key] = val

        # `include` pulls in another recipe's sources. Resolved after
        # the whole file is read so an include can appear anywhere.
        base = os.path.dirname(path)
        for inc in list(self.lists["include"]):
            sub = Spec(os.path.join(base, inc if inc.endswith(".spec")
                                    else inc + ".spec"))
            # Included sources come FIRST, so a recipe that includes
            # another can override a scalar but only append sources.
            self.lists["source"] = sub.lists["source"] + self.lists["source"]
            # Repos merge by name, with the including recipe winning --
            # that is what lets one recipe include another and pin a
            # different upstream ref.
            have = set(r.split()[0] for r in self.lists["repo"] if r.split())
            self.lists["repo"] = [r for r in sub.lists["repo"]
                                  if r.split() and r.split()[0] not in have
                                  ] + self.lists["repo"]
            for k, v in sub.values.items():
                self.values.setdefault(k, v)

    def get(self, key, default=None, required=False):
        if key in self.values:
            return self.values[key]
        if required:
            raise SpecError("%s: missing required key `%s`" % (self.path, key))
        return default

    def getint(self, key, default=None, required=False):
        v = self.get(key, None, required)
        if v is None:
            return default
        try:
            return int(v, 0)
        except ValueError:
            raise SpecError("%s: `%s` is not a number: %s"
                            % (self.path, key, v))

    def getlist(self, key):
        return list(self.lists.get(key, []))

    def repos(self):
        """Parse each `repo =` line into {name: {url, ref}}.

        Shape:  repo = <name> <url> [ref=<commit-or-tag>]
        """
        out = {}
        for line in self.lists["repo"]:
            parts = line.split()
            if len(parts) < 2:
                raise SpecError("%s: `repo` needs <name> <url>: %s"
                                % (self.path, line))
            name, url = parts[0], parts[1]
            ref = None
            for kv in parts[2:]:
                if kv.startswith("ref="):
                    ref = kv[4:]
                else:
                    raise SpecError("%s: unknown repo option `%s`"
                                    % (self.path, kv))
            out[name] = {"url": url, "ref": ref}
        return out

    def fetches(self):
        """Parse each `fetch =` line into a download step.

        Shape:  fetch = <dest> <url> [alt=<url>] [sha256=..]
                                     [unpack=auto|yes|no] [strip=N]
                                     [lfs=<repo>/<path>]

        `alt=` is a second URL, tried if the first fails. Upstream
        download paths rot -- Project Gutenberg has moved its at least
        once -- and a recipe that carries both spellings keeps working
        across the change instead of needing an edit.

        `unpack=auto` decides by looking at what arrived, not at the
        filename: PG serves plain text from URLs ending .txt.utf-8.

        <dest> is relative to the pack's fetch root. With unpack, dest
        is the DIRECTORY the archive expands into; without, it is the
        file itself.

        `lfs=` marks a file that lives in a git repo as an LFS object
        rather than at a URL -- Wikipedia is a 137MB one. It is copied
        out of the checkout after `git lfs pull`, with a clear error if
        git-lfs is not installed.
        """
        import shlex
        out = []
        for line in self.lists["fetch"]:
            parts = shlex.split(line)
            if len(parts) < 2:
                raise SpecError("%s: `fetch` needs <dest> <url|lfs=..>: %s"
                                % (self.path, line))
            step = {"dest": parts[0], "url": None, "sha256": None,
                    "unpack": "auto", "strip": "auto", "lfs": None,
                    "alt": None}
            rest = parts[1:]
            if "=" not in rest[0]:
                step["url"] = rest[0]
                rest = rest[1:]
            for kv in rest:
                if "=" not in kv:
                    raise SpecError("%s: fetch option `%s` is not key=value"
                                    % (self.path, kv))
                k, v = kv.split("=", 1)
                if k not in step:
                    raise SpecError("%s: unknown fetch option `%s`"
                                    % (self.path, k))
                if k == "strip" and v != "auto":
                    step[k] = int(v)
                else:
                    step[k] = v
            if not step["url"] and not step["lfs"]:
                raise SpecError("%s: fetch needs a url or lfs=: %s"
                                % (self.path, line))
            out.append(step)
        return out

    def sources(self):
        """Parse each `source =` line into (adapter, path, options).

        Shape:  source = <adapter> <path> [key=value ...]

        e.g.    source = codex   @ark/data/arklite/codex.tgz
                source = mdtree  ../../docs            prefix=docs
                source = onefile cia/35830.txt  title="World Factbook"

        Split with shlex so a value can contain spaces when quoted.
        Plain .split() was enough until the first option whose value
        was a title, at which point `title=CIA World Factbook` became
        three tokens and the parser complained about `World`.
        """
        import shlex
        out = []
        for line in self.lists["source"]:
            parts = shlex.split(line)
            if len(parts) < 2:
                raise SpecError("%s: `source` needs at least "
                                "<adapter> <path>: %s" % (self.path, line))
            adapter, loc = parts[0], parts[1]
            opts = {}
            for kv in parts[2:]:
                if "=" not in kv:
                    raise SpecError("%s: source option `%s` is not key=value"
                                    % (self.path, kv))
                k, v = kv.split("=", 1)
                opts[k] = v
            out.append((adapter, loc, opts))
        return out
