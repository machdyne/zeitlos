#!/usr/bin/env python3
"""
Fill every <dir>/po/<lang>.po from a JSON file of translations, without
touching anything else in them: the way translations made outside a PO
editor come in. docs/i18n.md.

    tools/i18n_fill.py LANG FILE.json      fill the empty entries
    tools/i18n_fill.py --list [DOMAIN...]  every key, as JSON, to translate

The JSON maps a key to its translation: the English for a plain string;
"context\\u0004English" for one with a context; "singular\\u0000plural"
for a plural, whose translation is a list of the language's forms.
Entries already translated are left as they are unless --force.
"""
import json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import i18n


def key_of(e):
    if e["plural"] is not None:
        return e["msgid"] + "\0" + e["plural"]
    if e["ctx"] is not None:
        return e["ctx"] + "\x04" + e["msgid"]
    return e["msgid"]


def main(argv):
    if len(argv) >= 2 and argv[1] == "--list":
        alld = i18n.domains()
        doms = argv[2:] or sorted(alld)
        out = {}
        for d in doms:
            _h, ents = i18n.read_po(i18n.pot_path(d, alld[d]))
            out[d] = [key_of(e) for e in ents]
        json.dump(out, sys.stdout, ensure_ascii=False, indent=0)
        return 0
    force = "--force" in argv
    argv = [a for a in argv if a != "--force"]
    lang, src = argv[1], argv[2]
    tr = json.load(open(src, encoding="utf-8"))
    nforms = i18n.LANGS[lang][3]
    filled = missing = 0
    for dom, d in sorted(i18n.domains().items()):
        path = i18n.po_path(lang, d)
        if not os.path.exists(path):
            continue
        header, ents = i18n.read_po(path)
        live = [e for e in ents if not e["obsolete"]]
        dead = [e for e in ents if e["obsolete"]]
        for e in live:
            if any(e["msgstr"]) and not force:
                continue
            k = key_of(e)
            if k not in tr:
                missing += 1
                continue
            v = tr[k]
            e["msgstr"] = v if isinstance(v, list) else [v]
            if e["plural"] is not None and len(e["msgstr"]) != nforms:
                raise SystemExit("%s: %r needs %d forms" % (lang, k, nforms))
            e["fuzzy"] = False
            filled += 1
        i18n.write_po(path, i18n.po_header(lang, dom), live, nforms, dead)
    print("%s: %d filled, %d still empty" % (lang, filled, missing))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
