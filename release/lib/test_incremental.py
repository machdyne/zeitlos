#!/usr/bin/env python3
#
# The incremental release workflow:
#
#   build v0.0.2 --targets lakritz_uart lakritz_langkatze
#   ship  v0.0.2
#   ...later...
#   build v0.0.2 --targets sergei_ml1
#   ship  v0.0.2
#
# The second build must ADD to dist/<version>/ rather than replace it,
# and the notes, README.txt and manifest must describe all three
# targets rather than only the last command's.
#
# Run from the repository root:
#   python3 release/lib/test_incremental.py
#

import sys as _sys

# No __pycache__. This tool runs once per invocation, so the bytecode
# cache saves nothing measurable -- and it is written into
# release/lib/, which made `git status` dirty as a side effect of
# running `zrelease check`, which then made `zrelease build` refuse to
# run. A check that its own tooling trips is a check people learn to
# pass --allow-dirty to, which defeats it.
#
# Belt and braces with the .gitignore entry: this way the file is never
# created, so a checkout that predates that entry behaves too.
_sys.dont_write_bytecode = True

import json
import os
import shutil

import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)

import importlib.machinery         # noqa: E402
import importlib.util               # noqa: E402
import io                           # noqa: E402
import contextlib                   # noqa: E402

import selftest                     # noqa: E402  (reuses its fake toolchain)
import build as build_mod           # noqa: E402

VERSION = "0.0.2"          # must match sw/common/zversion.h
OUT = os.path.join(ROOT, "release/dist", VERSION)

# This test writes into the REAL dist directory for 0.0.2, so it
# refuses to run if one already exists rather than trampling a release
# you have built for real.


def _load_zrelease():
    """Import the CLI script, which has no .py extension."""
    path = os.path.join(ROOT, "release/zrelease")
    spec = importlib.util.spec_from_loader(
        "zrelease", importlib.machinery.SourceFileLoader("zrelease", path))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


ZR = _load_zrelease()


class Result:
    def __init__(self, code, out):
        self.returncode = code
        self.stdout = out
        self.stderr = ""


def run_build(targets, extra=()):
    """Call cmd_build directly, with the make calls stubbed out.

    Going through argparse rather than constructing a Namespace by hand,
    so that a new flag with no default cannot make this test silently
    stop exercising the real path.
    """
    argv = ["build", VERSION, "--targets"] + list(targets) + \
        ["--allow-dirty", "--no-sdcard"] + list(extra)
    args = _mirror_parser().parse_args(argv)

    real = build_mod.run
    # selftest.fake_run lets the real mkzar.py through, via this hook.
    build_mod._real_run = real
    build_mod.run = selftest.fake_run(selftest.FAKE_PNR)
    buf = io.StringIO()
    try:
        with contextlib.redirect_stdout(buf):
            code = ZR.cmd_build(args)
    except SystemExit as e:
        code = e.code or 1
    finally:
        build_mod.run = real
    return Result(code, buf.getvalue())


def _mirror_parser():
    """The build subcommand's flags.

    Mirrors the CLI's own parser, which is local to its main(). If a new
    flag is added there without a default, cmd_build will raise
    AttributeError here rather than this test quietly exercising a
    stale path.
    """
    import argparse
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build")
    b.add_argument("version")
    b.add_argument("--targets", nargs="+")
    b.add_argument("--jobs", "-j", type=int)
    b.add_argument("--ark")
    b.add_argument("--no-sdcard", action="store_true")
    b.add_argument("--rebuild-sdcard", action="store_true")
    b.add_argument("--allow-mixed-commits", action="store_true")
    b.add_argument("--full-image", action="store_true")
    b.add_argument("--bump", action="store_true")
    b.add_argument("--allow-dirty", action="store_true")
    b.add_argument("--allow-drift", action="store_true")
    b.add_argument("--allow-timing-fail", action="store_true")
    b.add_argument("--strict-io-timing", action="store_true")
    b.add_argument("--keep-generated", action="store_true")
    b.add_argument("--dry-run", action="store_true")
    return p


def manifest():
    with open(os.path.join(OUT, "MANIFEST.json")) as f:
        return json.load(f)


def main():
    os.chdir(ROOT)
    failures = []
    created = [OUT, os.path.join(ROOT, "output", "releases")]
    for app in ("wm", "net", "repl", "term"):
        created.append(os.path.join(ROOT, "sw/apps", app, app + ".bin"))
    created.append(os.path.join(ROOT, "sw/os/kernel.bin"))

    if os.path.exists(OUT):
        print("%s already exists -- this test would overwrite it.\n"
              "Move it aside first." % os.path.relpath(OUT, ROOT))
        return 1

    try:
        print("== session 1: build two Lakritz targets ==")
        r = run_build(["lakritz_uart", "lakritz_langkatze"])
        if r.returncode != 0:
            print(r.stdout[-3000:], r.stderr[-2000:])
            failures.append("first build failed")
            return 1

        m1 = manifest()
        names1 = sorted(t["target"] for t in m1["targets"])
        print("   manifest targets: %s" % ", ".join(names1))
        if names1 != ["lakritz_langkatze", "lakritz_uart"]:
            failures.append("first build manifest wrong: %s" % names1)

        imgs1 = sorted(f for f in os.listdir(OUT) if f.endswith(".img"))
        print("   images: %s" % ", ".join(imgs1))

        with open(os.path.join(OUT, "README.txt")) as f:
            rt1 = f.read()

        print("\n== session 2: add sergei_ml1, same version ==")
        r = run_build(["sergei_ml1"])
        if r.returncode != 0:
            print(r.stdout[-3000:], r.stderr[-2000:])
            failures.append("second build failed")
            return 1
        if "already in" not in r.stdout:
            failures.append("second build did not report existing targets")
        print("   " + [l for l in r.stdout.splitlines()
                       if "target(s):" in l][0].strip())

        m2 = manifest()
        names2 = sorted(t["target"] for t in m2["targets"])
        print("   manifest targets: %s" % ", ".join(names2))
        if names2 != ["lakritz_langkatze", "lakritz_uart", "sergei_ml1"]:
            failures.append("second build LOST earlier targets: %s" % names2)

        imgs2 = sorted(f for f in os.listdir(OUT) if f.endswith(".img"))
        print("   images: %s" % ", ".join(imgs2))
        for old in imgs1:
            if old not in imgs2:
                failures.append("%s disappeared after the second build" % old)

        with open(os.path.join(OUT, "README.txt")) as f:
            rt2 = f.read()
        for name in ("lakritz_uart", "lakritz_langkatze", "sergei_ml1"):
            if "zeitlos-%s.img" % name not in rt2:
                failures.append("README.txt does not mention %s" % name)
        print("   README.txt describes all three")

        with open(os.path.join(OUT, "NOTES.md")) as f:
            notes = f.read()
        for name in ("lakritz_uart", "lakritz_langkatze", "sergei_ml1"):
            if name not in notes:
                failures.append("NOTES.md does not mention %s" % name)
        print("   NOTES.md describes all three")

        # SHA256SUMS must cover every asset, including the ones this run
        # did not touch -- ship uploads the whole directory.
        with open(os.path.join(OUT, "SHA256SUMS")) as f:
            summed = {l.split("  ", 1)[1].strip() for l in f if l.strip()}
        assets = {f for f in os.listdir(OUT)
                  if f not in ("SHA256SUMS", "NOTES.md")}
        if summed != assets:
            failures.append("SHA256SUMS covers %s, directory has %s"
                            % (sorted(summed), sorted(assets)))
        else:
            print("   SHA256SUMS covers all %d assets" % len(assets))

        print("\n== session 3: rebuild one target in place ==")
        before = os.path.getmtime(
            os.path.join(OUT, "zeitlos-lakritz_uart.img"))
        r = run_build(["lakritz_uart"])
        if r.returncode != 0:
            failures.append("rebuild failed")
        m3 = manifest()
        if sorted(t["target"] for t in m3["targets"]) != names2:
            failures.append("rebuilding one target changed the target list")
        after = os.path.getmtime(
            os.path.join(OUT, "zeitlos-lakritz_uart.img"))
        print("   target list unchanged; image rewritten: %s"
              % (after != before))

    finally:
        for p in created:
            if os.path.isdir(p):
                shutil.rmtree(p, ignore_errors=True)
            elif os.path.exists(p):
                os.unlink(p)

    print("\n==================")
    if failures:
        print("%d FAILURE(S):" % len(failures))
        for f in failures:
            print("  - %s" % f)
        return 1
    print("incremental release workflow works")
    return 0


if __name__ == "__main__":
    sys.exit(main())
