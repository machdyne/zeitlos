#
# Zeitlos release tooling -- the card's fixed locations.
#
# Reads sw/common/zpaths.h, the one registry of every fixed path on
# the card (docs/layout.md). The C code and the release tool both take
# their locations from that file, so the card the release writes and
# the paths the code opens cannot disagree.
#
# The header's format is deliberately one `#define Z_NAME "literal"`
# per line, nothing else (see the header's own comment), which is what
# lets this be a regular expression rather than a C preprocessor. Any
# `#define Z_DIR_`/`Z_PATH_` line that does not match exactly is an
# error here rather than a name that silently goes missing.
#
# The header is found next to THIS file's tree, not the tree being
# imaged: the tool and the registry it understands are one checkout.
#

import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
HEADER = os.path.normpath(os.path.join(HERE, "..", "..",
                                       "sw", "common", "zpaths.h"))

_DEF = re.compile(r'^#define\s+(Z_(?:DIR|PATH)_[A-Z0-9_]+)\s+"([^"\\]*)"\s*$')
_ANY = re.compile(r'^\s*#\s*define\s+Z_(?:DIR|PATH)_')


class PathsError(Exception):
    pass


def load(header=HEADER):
    """Every registry name -> its value, in file order."""
    out = {}
    with open(header, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not _ANY.match(line):
                continue
            m = _DEF.match(line)
            if not m:
                raise PathsError(
                    "%s:%d: not in the registry's format "
                    "(#define Z_NAME \"literal\", one per line): %s"
                    % (header, n, line))
            name, value = m.group(1), m.group(2)
            if name in out:
                raise PathsError("%s:%d: %s defined twice" % (header, n, name))
            if name.startswith("Z_DIR_") and len(value) > 1 and value.endswith("/"):
                raise PathsError("%s:%d: %s ends in '/'; directories are "
                                 "written without one" % (header, n, name))
            out[name] = value
    if not out:
        raise PathsError("%s: no Z_DIR_/Z_PATH_ definitions found" % header)
    return out


P = load()


def card(name, *parts):
    """A card destination as mkfatimg writes them: no leading slash.

    card("Z_DIR_APPS", "files") -> "apps/files". A registry value that
    is not absolute (the few files the code opens relative to the root,
    such as net.cfg) is taken as relative to the root, which is what
    FatFs makes of it.
    """
    if name not in P:
        raise PathsError("%s is not in %s" % (name, HEADER))
    base = P[name].strip("/")
    rest = "/".join(p.strip("/") for p in parts if p)
    if base and rest:
        return base + "/" + rest
    return base or rest


def dirname(name):
    """The directory a registry file lives in, as card() writes it."""
    return card(name).rpartition("/")[0]


def top_level_dirs():
    """Every first path component the registry uses: "apps", "user", ...

    For the literal lint in `zrelease check`: a string that starts with
    one of these is a card location and belongs in the registry.
    """
    tops = set()
    for v in P.values():
        if v.startswith("/") and len(v) > 1:
            tops.add(v[1:].split("/")[0])
    return sorted(tops)


# -- the lint: `zrelease check`, "card paths" --
#
# A card location written as a literal outside the registry is exactly
# the drift the registry exists to prevent: the release tool moves the
# file, the literal does not move with it, and the app looks in the old
# place on a card that no longer has it.
#
# Flagged, in sw/os, sw/apps and sw/common, comments stripped:
#
#   - any path in a literal that is a registry value (except "/", which
#     is also simply "the root" in a hundred places that mean nothing
#     else);
#   - any path in a literal that starts with a card top-level folder:
#     "/data", "/apps/x", "/data/%s", "could not read /sys/x". The
#     folders are the registry's own plus the layout's (docs/layout.md).
#
# Not scanned: host-side tests (they open host files, and their fixture
# paths are not card locations) and the Linux builds of bbs and fed
# (sw/apps/*/linux), which run on a Linux machine and use its paths.

LAYOUT_TOPS = ("apps", "data", "sys", "docs", "media", "home", "opt")

_SCAN = ("sw/os", "sw/apps", "sw/common")

# A path inside a literal: at its start, or after a space, a quote, a
# parenthesis or '='. So "could not read /sys/zeitlos.cfg" is caught as
# well as "/sys/zeitlos.cfg" -- a message names a place as surely as an
# open() does, and goes stale the same way -- while "a/b" and
# "https://host/x" are not paths on the card.
_PATHISH = re.compile(r"(?:^|(?<=[\s(`'\"=]))(/[A-Za-z0-9_.%-]+(?:/[A-Za-z0-9_.%*-]*)*)")
_SKIP_DIRS = {"tests", "test", "linux"}
_SKIP_FILE = re.compile(r"^(test_.*|.*_test|hosttest|port_host)\.[ch]$")


def _literals(text):
    """(line, literal) for every string literal, comments removed."""
    i, n, line = 0, len(text), 1
    while i < n:
        c = text[i]
        if c == "\n":
            line += 1
            i += 1
        elif text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            line += text.count("\n", i, j)
            i = j
        elif c == "'":
            j = i + 1
            while j < n and text[j] != "'":
                j += 2 if text[j] == "\\" else 1
            i = j + 1
        elif c == '"':
            j = i + 1
            while j < n and text[j] != '"' and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            yield line, text[i + 1:j]
            i = j + 1
        else:
            i += 1


def lint(root):
    """Problems, as strings; empty means the tree is clean."""
    values = {v for v in P.values() if v != "/"}
    tops = set(top_level_dirs()) | set(LAYOUT_TOPS)
    registry = os.path.relpath(HEADER, root)
    problems = []
    for base in _SCAN:
        for dp, dn, fn in os.walk(os.path.join(root, base)):
            dn[:] = sorted(d for d in dn if d not in _SKIP_DIRS)
            for f in sorted(fn):
                if not f.endswith((".c", ".h")) or _SKIP_FILE.match(f):
                    continue
                path = os.path.join(dp, f)
                rel = os.path.relpath(path, root)
                if rel == registry:
                    continue
                with open(path, encoding="utf-8", errors="replace") as fh:
                    text = fh.read()
                for line, lit in _literals(text):
                    for tok in _PATHISH.findall(lit):
                        top = tok[1:].split("/")[0]
                        if tok in values or top in tops:
                            problems.append(
                                "%s:%d: \"%s\" names a card location (%s) -- "
                                "name it in %s and use the name "
                                "(docs/layout.md)" % (rel, line, lit, tok, registry))
                            break
    return problems
