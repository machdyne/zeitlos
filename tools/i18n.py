#!/usr/bin/env python3
"""
Languages for Zeitlos: extract the strings programs mark, keep the
translations, build the catalogs jfont hands every app. docs/i18n.md.

    tools/i18n.py extract           <dir>/po/<domain>.pot from the sources,
                                    and every <dir>/po/<lang>.po brought
                                    up to date with it (new strings added
                                    empty, old ones kept, gone ones kept
                                    as obsolete #~)
    tools/i18n.py build OUTDIR      OUTDIR/<domain>/<lang>.zlc: each
                                    program's own catalog, for every
                                    language it has, and the
                                    pseudo-language xx
    tools/i18n.py stats             how much of each language is done
    tools/i18n.py check             the keys hash apart, and every
                                    translation keeps its printf formats
                                    (build checks both too)

-- domains --

A domain is a program's strings: sw/apps/<app>, for every app whose
sources call z_tr_init(); and "common", the shared library (sw/common).
A program looks in its own domain, then in common.

Everything of a domain lives with it: its translations in <dir>/po/
(sw/apps/files/po/de.po, sw/common/po/de.po), and on the card its
catalogs in its own folder, /data/<app>/lang/<lang>.zlc (common's in
/data/jfont/lang/). Adding a program adds its files and changes nothing
else; jfont finds every catalog at boot.

-- what is extracted --

    Z_("s")  ZS_("s")  Z_D-free N_("s")         msgid s
    Z_C("ctx", "s")                             msgctxt ctx, msgid s
    Z_N("one", "many", n)                       msgid one, msgid_plural many

with adjacent literals joined ("a" "b") and C escapes decoded. Only
literals: Z_D(x) translates something already marked elsewhere with
N_().

-- the pseudo-language xx --

Every string 40% longer and its letters accented, in brackets:
"Save as..." is "[Šåvé åš... ~~~~]". It is drawn entirely in Latin-9,
so the hardware fonts show it; an English string on a screen in xx is
one not wrapped, and a clipped bracket is a layout that counts instead
of measuring.
"""

import os
import re
import struct
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
def po_dir(d):
    """A domain's translations: beside its sources."""
    return os.path.join(d, "po")


def pot_path(name, d):
    return os.path.join(po_dir(d), name + ".pot")


def po_path(lang, d):
    return os.path.join(po_dir(d), lang + ".po")

# code: (English name, native name, plural rule, number of forms)
# Plural rules: sw/common/zi18n.h, Z_TR_PLURAL_*.
LANGS = {
    "de": ("German", "Deutsch", 1, 2),
    "fr": ("French", "Français", 2, 2),
    "es": ("Spanish", "Español", 1, 2),
    "it": ("Italian", "Italiano", 1, 2),
    "pt": ("Portuguese", "Português", 1, 2),
    "nl": ("Dutch", "Nederlands", 1, 2),
    "pl": ("Polish", "Polski", 5, 3),
    "cs": ("Czech", "Čeština", 4, 3),
    "sv": ("Swedish", "Svenska", 1, 2),
    "ru": ("Russian", "Русский", 3, 3),
    "uk": ("Ukrainian", "Українська", 3, 3),
    "ja": ("Japanese", "日本語", 0, 1),
    "xx": ("Pseudo", "[Pšéúðó]", 1, 2),
}
PLURAL_FORMS = {
    0: "nplurals=1; plural=0;",
    1: "nplurals=2; plural=(n != 1);",
    2: "nplurals=2; plural=(n > 1);",
    3: "nplurals=3; plural=(n%10==1 && n%100!=11 ? 0 : n%10>=2 && n%10<=4 && (n%100<10 || n%100>=20) ? 1 : 2);",
    4: "nplurals=3; plural=(n==1 ? 0 : n>=2 && n<=4 ? 1 : 2);",
    5: "nplurals=3; plural=(n==1 ? 0 : n%10>=2 && n%10<=4 && (n%100<10 || n%100>=20) ? 1 : 2);",
}


class I18nError(Exception):
    pass


# -- hashing (zi18n.h, z_tr_hash2) --

def fnv(data):
    h = 2166136261
    for b in data:
        h ^= b
        h = (h * 16777619) & 0xFFFFFFFF
    return h


def key_bytes(ctx, msgid, plural):
    if plural is not None:
        return msgid.encode() + b"\0" + plural.encode()
    if ctx is not None:
        return ctx.encode() + b"\x04" + msgid.encode()
    return msgid.encode()


# -- C literals --

_ESC = {"n": "\n", "t": "\t", "r": "\r", "\\": "\\", '"': '"', "'": "'", "0": "\0",
        "a": "\a", "b": "\b", "f": "\f", "v": "\v", "e": "\x1b", "?": "?"}


def c_unescape(s):
    out, i = [], 0
    raw = bytearray()
    while i < len(s):
        c = s[i]
        if c != "\\":
            raw += c.encode()
            i += 1
            continue
        n = s[i + 1]
        if n == "x":
            m = re.match(r"[0-9a-fA-F]+", s[i + 2:])
            raw.append(int(m.group(0), 16) & 0xFF)
            i += 2 + len(m.group(0))
        elif n in "01234567" and re.match(r"[0-7]{1,3}", s[i + 1:]).group(0) not in ("0",):
            m = re.match(r"[0-7]{1,3}", s[i + 1:])
            raw.append(int(m.group(0), 8) & 0xFF)
            i += 1 + len(m.group(0))
        else:
            raw += _ESC.get(n, n).encode()
            i += 2
    return raw.decode("utf-8")


STR = r'"(?:[^"\\\n]|\\.)*"'
STRS = r'(?:' + STR + r'\s*)+'


def join_lits(text):
    return "".join(c_unescape(m[1:-1]) for m in re.findall(STR, text))


CALLS = [
    ("plain", re.compile(r'\b(?:Z_|ZS_|ZL_|N_)\(\s*(' + STRS + r')\)')),
    ("ctx", re.compile(r'\b(?:Z_C|NC_|Z_DC|ZS_DC)\(\s*(' + STRS + r'),\s*(' + STRS + r')\)')),
    ("plural", re.compile(r'\bZS?_N\(\s*(' + STRS + r'),\s*(' + STRS + r'),')),
]


def strip_comments(src):
    # Comments out, string literals kept (a // inside a string is not one).
    out, i, n = [], 0, len(src)
    while i < n:
        c = src[i]
        if c == '"' or c == "'":
            q = c
            j = i + 1
            while j < n and src[j] != q:
                j += 2 if src[j] == "\\" else 1
            out.append(src[i:j + 1])
            i = j + 1
        elif src.startswith("//", i):
            j = src.find("\n", i)
            i = n if j < 0 else j
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            out.append("\n" * src.count("\n", i, n if j < 0 else j))
            i = n if j < 0 else j + 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def extract_file(path):
    """[(ctx, msgid, plural, 'file:line')]"""
    src = strip_comments(open(path, encoding="utf-8", errors="replace").read())
    found = []
    for kind, rx in CALLS:
        for m in rx.finditer(src):
            line = src.count("\n", 0, m.start()) + 1
            ref = "%s:%d" % (os.path.relpath(path, ROOT), line)
            if kind == "plain":
                found.append((None, join_lits(m.group(1)), None, ref, m.start()))
            elif kind == "ctx":
                found.append((join_lits(m.group(1)), join_lits(m.group(2)), None, ref, m.start()))
            else:
                found.append((None, join_lits(m.group(1)), join_lits(m.group(2)), ref, m.start()))
    found.sort(key=lambda f: f[4])
    return [f[:4] for f in found]


def sources(d):
    for dirpath, dirs, files in os.walk(d):
        dirs[:] = sorted(x for x in dirs if x not in ("tests", "test"))
        for f in sorted(files):
            if f.endswith((".c", ".h")):
                yield os.path.join(dirpath, f)


def domains(root=ROOT):
    """{domain: source dir}: common, and every app that calls z_tr_init()."""
    out = {"common": os.path.join(root, "sw", "common")}
    apps = os.path.join(root, "sw", "apps")
    for app in sorted(os.listdir(apps)):
        d = os.path.join(apps, app)
        if not os.path.isdir(d):
            continue
        for f in sources(d):
            if "z_tr_init(" in open(f, encoding="utf-8", errors="replace").read():
                out[app] = d
                break
    return out


def extract_domain(d, name):
    """{key: entry} in first-seen order; entry = dict(ctx, msgid, plural, refs)."""
    keys = {}
    for f in sources(d):
        if name == "common" and os.sep + "tests" + os.sep in f:
            continue
        if os.path.basename(f) in ("zi18n.h", "zi18n.c"):
            continue
        for ctx, msgid, plural, ref in extract_file(f):
            if not msgid:
                continue
            k = (ctx, msgid, plural)
            e = keys.setdefault(k, {"ctx": ctx, "msgid": msgid, "plural": plural, "refs": []})
            e["refs"].append(ref)
    return keys


# -- PO files --

def po_quote(s):
    s = s.replace("\\", "\\\\").replace('"', '\\"').replace("\t", "\\t").replace("\r", "\\r")
    s = s.replace("\x1b", "\\033")
    if "\n" in s and s != "\n":
        parts = s.split("\n")
        lines = [p + "\\n" for p in parts[:-1]] + ([parts[-1]] if parts[-1] else [])
        return '""\n' + "\n".join('"%s"' % l for l in lines)
    return '"%s"' % s.replace("\n", "\\n")


def po_unquote(lines):
    s = "".join(l.strip()[1:-1] for l in lines)
    out, i = [], 0
    while i < len(s):
        if s[i] == "\\" and i + 1 < len(s):
            n = s[i + 1]
            if n in "01234567":
                m = re.match(r"[0-7]{1,3}", s[i + 1:])
                out.append(chr(int(m.group(0), 8)))
                i += 1 + len(m.group(0))
                continue
            out.append({"n": "\n", "t": "\t", "r": "\r", "\\": "\\", '"': '"'}.get(n, n))
            i += 2
        else:
            out.append(s[i])
            i += 1
    return "".join(out)


def read_po(path):
    """(header text, [entries]); entry = dict(ctx, msgid, plural, msgstr (list), fuzzy, obsolete, comments)."""
    entries, cur, field = [], None, None
    buf = {}

    def flush():
        nonlocal cur
        if cur is not None and "msgid" in buf:
            cur["ctx"] = po_unquote(buf["msgctxt"]) if "msgctxt" in buf else None
            cur["msgid"] = po_unquote(buf["msgid"])
            cur["plural"] = po_unquote(buf["msgid_plural"]) if "msgid_plural" in buf else None
            if cur["plural"] is not None:
                n = 0
                strs = []
                while ("msgstr[%d]" % n) in buf:
                    strs.append(po_unquote(buf["msgstr[%d]" % n]))
                    n += 1
                cur["msgstr"] = strs
            else:
                cur["msgstr"] = [po_unquote(buf.get("msgstr", ['""']))]
            entries.append(cur)
        cur = None
        buf.clear()

    for raw in open(path, encoding="utf-8"):
        line = raw.rstrip("\n")
        obsolete = line.startswith("#~")
        if obsolete:
            line = line[2:].lstrip()
        if not line.strip():
            flush()
            continue
        if line.startswith("#") and not obsolete:
            if cur is not None and "msgid" in buf:
                flush()
            if cur is None:
                cur = {"fuzzy": False, "obsolete": False, "comments": []}
            if line.startswith("#,") and "fuzzy" in line:
                cur["fuzzy"] = True
            cur["comments"].append(line)
            continue
        m = re.match(r'(msgctxt|msgid_plural|msgid|msgstr(?:\[\d+\])?)\s+(".*")$', line)
        if m:
            if m.group(1) in ("msgctxt",) or (m.group(1) == "msgid" and "msgid" in buf):
                if "msgid" in buf:
                    flush()
            if cur is None:
                cur = {"fuzzy": False, "obsolete": False, "comments": []}
            cur["obsolete"] = cur["obsolete"] or obsolete
            field = m.group(1)
            buf[field] = [m.group(2)]
            continue
        if line.strip().startswith('"') and field:
            buf[field].append(line.strip())
            continue
        raise I18nError("%s: cannot read line: %r" % (path, raw))
    flush()
    header = ""
    if entries and entries[0]["msgid"] == "" and entries[0]["ctx"] is None:
        header = entries.pop(0)["msgstr"][0]
    return header, entries


def entry_text(e, nforms, obsolete=False):
    pre = "#~ " if obsolete else ""
    out = []
    if not obsolete:
        for c in e.get("comments", []):
            if c.startswith("#.") or c.startswith("# "):
                out.append(c)
        for r in e.get("refs", [])[:6]:
            out.append("#: " + r)
        if e.get("fuzzy"):
            out.append("#, fuzzy")
        if "%" in e["msgid"]:
            out.append("#, c-format")

    def field(name, val):
        q = po_quote(val).split("\n")
        return [pre + name + " " + q[0]] + [pre + x for x in q[1:]]

    if e["ctx"] is not None:
        out += field("msgctxt", e["ctx"])
    out += field("msgid", e["msgid"])
    if e["plural"] is not None:
        out += field("msgid_plural", e["plural"])
        strs = list(e.get("msgstr") or [])
        strs += [""] * (nforms - len(strs))
        for i in range(nforms):
            out += field("msgstr[%d]" % i, strs[i])
    else:
        out += field("msgstr", (e.get("msgstr") or [""])[0])
    return "\n".join(out)


def po_header(lang, domain):
    if lang is None:
        return ("Project-Id-Version: Zeitlos %s\\n" % domain).replace("\\n", "\n") + \
            "Content-Type: text/plain; charset=UTF-8\nContent-Transfer-Encoding: 8bit\n"
    en, native, rule, n = LANGS[lang]
    return ("Project-Id-Version: Zeitlos %s\nLanguage: %s\n"
            "Content-Type: text/plain; charset=UTF-8\nContent-Transfer-Encoding: 8bit\n"
            "Plural-Forms: %s\n" % (domain, lang, PLURAL_FORMS[rule]))


def write_po(path, header, entries, nforms, obsolete=()):
    parts = ['msgid ""\n' + "msgstr " + po_quote(header)]
    for e in entries:
        parts.append(entry_text(e, nforms))
    for e in obsolete:
        parts.append(entry_text(e, nforms, obsolete=True))
    text = "\n\n".join(parts) + "\n"
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if os.path.exists(path) and open(path, encoding="utf-8").read() == text:
        return False
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    return True


def languages():
    """Every language there is a translation into (xx is made, not kept)."""
    return sorted(l for l in LANGS if l != "xx")


def cmd_extract(root=ROOT):
    doms = domains(root)
    changed = 0
    for name, d in doms.items():
        keys = extract_domain(d, name)
        ents = list(keys.values())
        changed += write_po(pot_path(name, d), po_header(None, name), ents, 2)
        for lang in languages():
            path = po_path(lang, d)
            old = {}
            if os.path.exists(path):
                _h, olds = read_po(path)
                for e in olds:
                    old[(e["ctx"], e["msgid"], e["plural"])] = e
            nforms = LANGS[lang][3]
            new = []
            for k, e in keys.items():
                o = old.pop(k, None)
                ne = dict(e)
                ne["msgstr"] = o["msgstr"] if o else []
                ne["fuzzy"] = o["fuzzy"] if o else False
                ne["comments"] = [c for c in (o or {}).get("comments", []) if c.startswith("# ")]
                new.append(ne)
            obsolete = [o for o in old.values() if any(s for s in o["msgstr"])]
            changed += write_po(path, po_header(lang, name), new, nforms, obsolete)
    print("i18n: %d domains (%s); %d files changed" % (len(doms), ", ".join(doms), changed))


# -- checks --

FMT = re.compile(r"%(?:%|[-+ #0]*(?:\*|\d+)?(?:\.(?:\*|\d+))?(?:hh|h|ll|l|z|j|t|L)?[diouxXeEfFgGcspn])")


def formats(s):
    return [f for f in FMT.findall(s) if f != "%%"]


def check_entry(dom, lang, e):
    """Problems with one translated entry (empty list: fine)."""
    probs = []
    srcs = [e["msgid"]] if e["plural"] is None else [e["msgid"], e["plural"]]
    want = formats(srcs[-1])
    for s in e["msgstr"]:
        if not s:
            continue
        if formats(s) != want and not (e["plural"] is not None and formats(s) == formats(srcs[0])):
            probs.append("%s/%s: %r: formats %s, the English has %s" % (lang, dom, e["msgid"], formats(s), want))
    return probs


def collisions(keysets):
    """Keys of a domain (with common's) that hash the same."""
    probs = []
    common = keysets.get("common", {})
    for dom, keys in keysets.items():
        seen = {}
        pool = list(keys) + ([k for k in common if dom != "common"])
        for k in pool:
            h = fnv(key_bytes(*k))
            if h in seen and seen[h] != k:
                probs.append("%s: %r and %r have the same hash %08x" % (dom, seen[h], k, h))
            seen[h] = k
    return probs


# -- the pseudo-language --

ACC = dict(zip("aeiouyAEIOUYcCnNsSzZ", "åéîõüÿÅÉÎÕÜŸçÇñÑšŠžŽ"))


def pseudo(s):
    out, i = [], 0
    for m in re.finditer(r"%[-+ #0]*\d*(?:\.\d+)?(?:hh|h|ll|l|z)?[diouxXeEfFgGcsp%]|\\.|\n|\t", s):
        out.append("".join(ACC.get(c, c) for c in s[i:m.start()]))
        out.append(m.group(0))
        i = m.end()
    out.append("".join(ACC.get(c, c) for c in s[i:]))
    body = "".join(out)
    letters = sum(c.isalpha() for c in s)
    pad = "~" * max(1, (letters * 2 + 4) // 5)
    if s.endswith("\n"):
        return "[" + body[:-1] + " " + pad + "]\n"
    return "[" + body + " " + pad + "]"


# -- the catalog (zi18n.h) --

def build_catalog(lang, rule, doms):
    """doms: {domain: {key_tuple: [forms]}} -> bytes"""
    dlist = sorted(doms.items(), key=lambda kv: fnv(kv[0].encode()))
    entries, strings = [], bytearray()
    table = []
    nd = len(dlist)
    n_entries = sum(len(v) for _d, v in dlist)
    str_base = 24 + 12 * nd + 8 * n_entries
    for name, keys in dlist:
        first = len(entries)
        rows = []
        for k, forms in keys.items():
            h = fnv(key_bytes(*k))
            if k[2] is not None:
                blob = bytes([len(forms)]) + b"".join(f.encode() + b"\0" for f in forms)
            else:
                blob = forms[0].encode() + b"\0"
            rows.append((h, blob))
        rows.sort()
        for h, blob in rows:
            entries.append((h, str_base + len(strings)))
            strings += blob
        table.append((fnv(name.encode()), first, len(rows)))
    out = bytearray(b"ZLC1")
    out += struct.pack("<HBB", 1, rule, 0)
    out += lang.encode().ljust(8, b"\0")[:8]
    out += struct.pack("<II", nd, 0)
    for h, first, count in table:
        out += struct.pack("<III", h, first, count)
    for h, off in entries:
        out += struct.pack("<II", h, off)
    out += strings
    return bytes(out)


def gather(root=ROOT):
    """({domain: keys}, {lang: {domain: {key: forms}}}, problems)"""
    doms = domains(root)
    keysets = {n: extract_domain(d, n) for n, d in doms.items()}
    probs = collisions(keysets)
    langs = {}
    for lang in languages():
        per = {}
        for dom, keys in keysets.items():
            path = po_path(lang, doms[dom])
            if not os.path.exists(path):
                continue
            _h, ents = read_po(path)
            got = {}
            for e in ents:
                k = (e["ctx"], e["msgid"], e["plural"])
                if e["obsolete"] or e["fuzzy"] or k not in keys or not any(e["msgstr"]):
                    continue
                if not all(e["msgstr"]):
                    continue
                p = check_entry(dom, lang, e)
                if p:
                    probs += p
                    continue
                got[k] = e["msgstr"]
            if got:
                per[dom] = got
        langs[lang] = per
    xx = {}
    for dom, keys in keysets.items():
        got = {}
        for k in keys:
            ctx, msgid, plural = k
            got[k] = [pseudo(msgid), pseudo(plural)] if plural is not None else [pseudo(msgid)]
        if got:
            xx[dom] = got
    langs["xx"] = xx
    return keysets, langs, probs


def cmd_build(outdir, root=ROOT):
    """OUTDIR/<domain>/<lang>.zlc, one catalog per program and language
    (only where there is something translated). Returns [(domain, lang,
    path)]. OUTDIR is emptied first: it holds nothing else."""
    import shutil
    keysets, langs, probs = gather(root)
    hard = [p for p in probs if "same hash" in p]
    for p in probs:
        print("i18n: " + p, file=sys.stderr)
    if hard:
        raise I18nError("hash collisions: change one of the strings")
    if os.path.isdir(outdir):
        shutil.rmtree(outdir)
    made = []
    for lang, per in sorted(langs.items()):
        for dom, keys in sorted(per.items()):
            data = build_catalog(lang, LANGS[lang][2], {dom: keys})
            path = os.path.join(outdir, dom, lang + ".zlc")
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(data)
            made.append((dom, lang, path))
        n = sum(len(v) for v in per.values())
        print("i18n: %s: %d strings in %d catalogs" % (lang, n, len(per)))
    return made


def cmd_stats(root=ROOT):
    keysets, langs, probs = gather(root)
    total = {d: len(k) for d, k in keysets.items()}
    allk = sum(total.values())
    print("%-8s %s" % ("", " ".join("%6s" % l for l in sorted(langs) if l != "xx")))
    for d in sorted(total):
        row = []
        for l in sorted(langs):
            if l == "xx":
                continue
            row.append("%5d%%" % (100 * len(langs[l].get(d, {})) // max(1, total[d])))
        print("%-8s %s   (%d strings)" % (d, " ".join(row), total[d]))
    print("%d strings in %d domains" % (allk, len(total)))
    for p in probs:
        print("problem: " + p)


def cmd_check(root=ROOT):
    _k, _l, probs = gather(root)
    for p in probs:
        print("i18n: " + p)
    return 1 if probs else 0


def main(argv):
    if len(argv) < 2 or argv[1] not in ("extract", "build", "stats", "check"):
        sys.stderr.write(__doc__)
        return 2
    try:
        if argv[1] == "extract":
            cmd_extract()
        elif argv[1] == "build":
            if len(argv) < 3:
                sys.stderr.write("i18n: build OUTDIR\n")
                return 2
            cmd_build(argv[2])
        elif argv[1] == "stats":
            cmd_stats()
        else:
            return cmd_check()
    except I18nError as e:
        sys.stderr.write("i18n: %s\n" % e)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
