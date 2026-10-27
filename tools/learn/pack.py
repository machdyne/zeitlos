#!/usr/bin/env python3
#
# Zeitlos learn -- builds course packs (docs/learn.md, "Packs").
#
#   ./tools/learn/pack.py build tools/learn/packs/edu-zeitlos.cfg
#   ./tools/learn/pack.py list  tools/learn/packs/edu-zeitlos.cfg
#
# A course pack is courses, copied as they are: learn reads course.cfg,
# the numbered folders and the lessons from the card exactly as they are
# in the source, so nothing is renamed or converted. What the build adds
# is the checking and the record:
#
#   1. learncheck over every course in the pack (with --release when the
#      recipe says so); an error stops the build.
#   2. every card path checked against what FatFs on the board opens
#      (release/lib/mkfatimg.py's check_card_path), before the image is.
#   3. the tree laid out as the card has it, under out/<pack>/:
#        out/<pack>/opt/ark/<pack>/<course>/...     the courses
#        out/<pack>/opt/ark/<pack>/lib/*.scm        the pack's figure library, if any
#        out/<pack>/opt/ark/<pack>/pack.cfg         what was built, from what
#
# The same recipe and the same sources make the same bytes: files are
# taken in sorted order and pack.cfg holds no time, only a hash of the
# contents. release/ ships out/<pack> like an ask pack (one half: the
# courses; ask's index of them is a later step).
#
# The Ark's edu packs are built by this tool too, from the ark clone
# that tools/ask/lib/fetch.py keeps for ask (tools/ask/.cache/ark, or
# $ASK_CACHE/ark): the courses live in the ark repo under edu/, the
# builder here. A recipe says so with
#
#     repo   = ark https://github.com/machdyne/ark      (ref=<commit> to pin)
#     source = @ark/edu
#
# `build --update` fetches the clone first; otherwise a checkout that is
# there is used as it is (a local edit is a fine way to test a course).

import argparse
import hashlib
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "release", "lib"))

LEARNCHECK = "/tmp/learncheck"
SKIP = (".", "#", "~")


def read_recipe(path):
    keys, lists = {}, {}
    with open(path) as f:
        for n, line in enumerate(f, 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if "=" not in line:
                raise SystemExit("%s:%d: not key = value" % (path, n))
            k, v = (x.strip() for x in line.split("=", 1))
            if k in ("course", "repo"):
                lists.setdefault(k, []).append(v)
            else:
                keys[k] = v
    for k in ("name", "version", "source"):
        if k not in keys:
            raise SystemExit("%s: no %s" % (path, k))
    if not keys["name"].startswith("edu"):
        raise SystemExit("%s: a course pack's name starts with edu (learn looks "
                         "in /opt/ark/edu*)" % path)
    keys["courses"] = lists.get("course", [])
    keys["repos"] = {}
    for v in lists.get("repo", []):
        parts = v.split()
        if len(parts) < 2:
            raise SystemExit("%s: repo = NAME URL [ref=COMMIT]" % path)
        ref = None
        for x in parts[2:]:
            if x.startswith("ref="):
                ref = x[4:]
        keys["repos"][parts[0]] = {"url": parts[1], "ref": ref}
    keys["recipe_dir"] = os.path.dirname(os.path.abspath(path))
    if not keys["source"].startswith("@"):
        keys["source"] = os.path.normpath(os.path.join(keys["recipe_dir"], keys["source"]))
    return keys


def fetch_source(r, update=False):
    """`source = @ark/edu`: the repo checked out (tools/ask's cache), the path in it."""
    if not r["source"].startswith("@"):
        return r
    sys.path.insert(0, os.path.join(ROOT, "tools", "ask", "lib"))
    import fetch
    root = fetch.cache_root(os.path.join(ROOT, "tools", "ask"))
    repos = {}
    for name, x in r["repos"].items():
        path, commit = fetch.ensure(name, x["url"], ref=x["ref"], root=root, update=update)
        repos[name] = {"path": path, "commit": commit}
        r.setdefault("commits", []).append("%s %s" % (name, commit or "(not a checkout)"))
    try:
        r["source"] = fetch.resolve(r["source"], repos, r["recipe_dir"])
    except fetch.FetchError as e:
        raise SystemExit(str(e))
    if not os.path.isdir(r["source"]):
        raise SystemExit("%s: not there (is the checkout older than its edu/?)" % r["source"])
    return r


def courses_of(r):
    src = r["source"]
    have = sorted(d for d in os.listdir(src)
                  if os.path.isfile(os.path.join(src, d, "course.cfg")))
    want = r["courses"] or have
    missing = [c for c in want if c not in have]
    if missing:
        raise SystemExit("not in %s: %s" % (src, ", ".join(missing)))
    if r["name"] != "edu-zeitlos":
        own = [c for c in want if c.startswith("zeit-")]
        if own:
            raise SystemExit("zeit- courses belong to edu-zeitlos, built from the "
                             "Zeitlos tree: %s" % ", ".join(own))
    return sorted(want)


def files_of(top):
    out = []
    for dp, dn, fn in os.walk(top):
        dn[:] = sorted(d for d in dn if not d.startswith(SKIP))
        for f in sorted(fn):
            if f.startswith(SKIP) or f.endswith("~"):
                continue
            out.append(os.path.join(dp, f))
    return out


def course_version(path):
    with open(os.path.join(path, "course.cfg")) as f:
        for line in f:
            k, _, v = line.partition("=")
            if k.strip() == "version":
                return v.strip()
    return "?"


def learncheck(paths, release):
    subprocess.run(["make", "-s", "-C", os.path.join(HERE, "learncheck")], check=True)
    cmd = [LEARNCHECK] + (["--release"] if release else []) + paths
    r = subprocess.run(cmd)
    if r.returncode:
        raise SystemExit("learncheck found errors: the pack is not built")


def cmd_list(a):
    r = fetch_source(read_recipe(a.recipe), a.update)
    for c in courses_of(r):
        print("%-24s version %s" % (c, course_version(os.path.join(r["source"], c))))


def cmd_build(a):
    r = fetch_source(read_recipe(a.recipe), a.update)
    name, src = r["name"], r["source"]
    courses = courses_of(r)
    learncheck([os.path.join(src, c) for c in courses], r.get("release", "no") == "yes")
    # every term taught before it is used (warnings; the build goes on)
    subprocess.run([sys.executable, os.path.join(HERE, "terms.py")] + [os.path.join(src, c) for c in courses])

    import mkfatimg
    card = mkfatimg.zcard("Z_DIR_ARK", name)          # opt/ark/<name>
    out = os.path.join(HERE, "out", name)
    pick = [(c, os.path.join(src, c)) for c in courses]
    if os.path.isdir(os.path.join(src, "lib")):
        pick.append(("lib", os.path.join(src, "lib")))

    plan = []           # (card path, source)
    for top, path in pick:
        for f in files_of(path):
            rel = os.path.relpath(f, src).replace(os.sep, "/")
            plan.append(("/%s/%s" % (card, rel), f))
    for dest, _src in plan:
        mkfatimg.check_card_path(dest)

    h = hashlib.sha256()
    for dest, f in plan:
        h.update(dest.encode() + b"\0")
        with open(f, "rb") as fh:
            h.update(fh.read())
        h.update(b"\0")

    tmp = out + ".part"
    shutil.rmtree(tmp, ignore_errors=True)
    for dest, f in plan:
        d = os.path.join(tmp, dest.lstrip("/"))
        os.makedirs(os.path.dirname(d), exist_ok=True)
        shutil.copyfile(f, d)
    lines = ["# made by tools/learn/pack.py from %s; do not edit" % os.path.relpath(a.recipe, ROOT),
             "name = %s" % name,
             "version = %s" % r["version"],
             "description = %s" % r.get("description", ""),
             "format = 1"]
    lines += ["source = %s" % c for c in r.get("commits", [])]
    lines += ["course = %s %s" % (c, course_version(os.path.join(src, c))) for c in courses]
    lines += ["files = %d" % len(plan), "hash = %s" % h.hexdigest()]
    with open(os.path.join(tmp, card, "pack.cfg"), "w") as f:
        f.write("\n".join(lines) + "\n")
    shutil.rmtree(out, ignore_errors=True)
    os.rename(tmp, out)
    size = sum(os.path.getsize(f) for _d, f in plan)
    print("pack %s %s: %d course%s, %d files, %.1f KB -> %s"
          % (name, r["version"], len(courses), "" if len(courses) == 1 else "s",
             len(plan) + 1, size / 1024, os.path.relpath(out, ROOT)))


def main():
    ap = argparse.ArgumentParser(description="build learn's course packs")
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name, fn in (("build", cmd_build), ("list", cmd_list)):
        p = sub.add_parser(name)
        p.add_argument("recipe")
        p.add_argument("--update", action="store_true", help="fetch the recipe's repos first")
        p.set_defaults(fn=fn)
    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
