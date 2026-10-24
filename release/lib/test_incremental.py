#!/usr/bin/env python3
#
# The incremental release workflow:
#
#   build vX --targets lakritz_gpio lakritz_langkatze
#   ship  vX
#   ...later...
#   build vX --targets sergei_ml1
#   ship  vX
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

# A version of the test's own, which no real release will ever use --
# like selftest.py's "0.0.2-selftest". It used to be "0.0.2", which had
# to equal sw/common/zversion.h and so broke, with a version-mismatch
# error, the day the header moved on. The header's own version would not
# do either: the test writes into release/dist/<version>/, and refuses to
# run if that exists -- so it would stop working after every real build.
# run_build() makes zrelease's version check see this one instead; the
# header is not touched.
VERSION = "0.0.0-inctest"
OUT = os.path.join(ROOT, "release/dist", VERSION)


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
    # An EMPTY target list means the CLI's own default, every target --
    # which is how a caller exercises the no---targets path. Passing
    # "--targets" with nothing after it is an argparse error, not that.
    argv = ["build", VERSION]
    if targets:
        argv += ["--targets"] + list(targets)
    argv += ["--allow-dirty"] + list(extra)
    if "--rebuild-sdcard" not in extra:
        argv.append("--no-sdcard")
    args = _mirror_parser().parse_args(argv)

    real = build_mod.run
    real_version = build_mod.read_version
    # selftest.fake_run lets the real mkzar.py through, via this hook.
    build_mod._real_run = real
    build_mod.run = selftest.fake_run(selftest.FAKE_PNR)
    # zrelease compares the requested version with the header's; the
    # header says the tree's real version, this test builds its own
    build_mod.read_version = lambda root: VERSION
    buf = io.StringIO()
    try:
        with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(buf):
            code = ZR.cmd_build(args)
    except SystemExit as e:
        code = e.code or 1
    finally:
        build_mod.run = real
        build_mod.read_version = real_version
    return Result(code, buf.getvalue())


def _mirror_parser():
    """The CLI's own parser (zrelease's make_parser()), so a flag added
    there is a flag this test passes through -- with its default --
    rather than an AttributeError in cmd_build or a stale path."""
    return ZR.make_parser()


def manifest():
    with open(os.path.join(OUT, "MANIFEST.json")) as f:
        return json.load(f)


def main():
    os.chdir(ROOT)
    failures = []
    created = [OUT, os.path.join(ROOT, "output", "releases")]
    # the fakes overwrite every core app and the kernel: the developer's
    # own builds are kept aside and put back (selftest.py does the same)
    bins = [os.path.join(ROOT, "sw/apps", a, a + ".bin") for a in selftest.fake_apps()]
    bins.append(os.path.join(ROOT, "sw/os/kernel.bin"))
    created.extend(bins)
    saved = selftest.keep_aside(bins)

    if os.path.exists(OUT):
        print("%s already exists -- this test would overwrite it.\n"
              "Move it aside first." % os.path.relpath(OUT, ROOT))
        return 1

    try:
        print("== session 1: build two Lakritz targets ==")
        r = run_build(["lakritz_gpio", "lakritz_langkatze"])
        if r.returncode != 0:
            print(r.stdout[-3000:], r.stderr[-2000:])
            failures.append("first build failed")
            return 1

        m1 = manifest()
        names1 = sorted(t["target"] for t in m1["targets"])
        print("   manifest targets: %s" % ", ".join(names1))
        if names1 != ["lakritz_gpio", "lakritz_langkatze"]:
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
        if names2 != ["lakritz_gpio", "lakritz_langkatze", "sergei_ml1"]:
            failures.append("second build LOST earlier targets: %s" % names2)

        imgs2 = sorted(f for f in os.listdir(OUT) if f.endswith(".img"))
        print("   images: %s" % ", ".join(imgs2))
        for old in imgs1:
            if old not in imgs2:
                failures.append("%s disappeared after the second build" % old)

        with open(os.path.join(OUT, "README.txt")) as f:
            rt2 = f.read()
        for name in ("lakritz_gpio", "lakritz_langkatze", "sergei_ml1"):
            if "zeitlos-%s.img" % name not in rt2:
                failures.append("README.txt does not mention %s" % name)
        print("   README.txt describes all three")

        with open(os.path.join(OUT, "NOTES.md")) as f:
            notes = f.read()
        for name in ("lakritz_gpio", "lakritz_langkatze", "sergei_ml1"):
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
            os.path.join(OUT, "zeitlos-lakritz_gpio.img"))
        r = run_build(["lakritz_gpio"])
        if r.returncode != 0:
            failures.append("rebuild failed")
        m3 = manifest()
        if sorted(t["target"] for t in m3["targets"]) != names2:
            failures.append("rebuilding one target changed the target list")
        after = os.path.getmtime(
            os.path.join(OUT, "zeitlos-lakritz_gpio.img"))
        print("   target list unchanged; image rewritten: %s"
              % (after != before))

        print("\n== session 4: a run that stops part way ==")
        #
        # The case this exists for: build three targets, have the
        # second one miss timing. The first one's images are in dist/
        # and `ship` uploads the directory, so they go out regardless --
        # what must not happen is the MANIFEST forgetting them, because
        # the next `build` merges into whatever the manifest says and
        # would then describe a three-target release as a one-target
        # one.
        shutil.rmtree(OUT, ignore_errors=True)

        real_bt = build_mod.build_target
        order = []

        def stops_on_the_second(root, t, *a, **k):
            order.append(t.name)
            if len(order) == 2:
                raise build_mod.BuildError(
                    "%s missed timing on clk_48 (simulated)" % t.name)
            return real_bt(root, t, *a, **k)

        build_mod.build_target = stops_on_the_second
        ZR.build_mod.build_target = stops_on_the_second
        try:
            run_build(["lakritz_gpio", "lakritz_langkatze", "sergei_ml1"])
            failures.append("the simulated timing failure did not stop the build")
        except build_mod.BuildError:
            pass
        finally:
            build_mod.build_target = real_bt
            ZR.build_mod.build_target = real_bt

        if not os.path.exists(os.path.join(OUT, "MANIFEST.json")):
            failures.append(
                "no MANIFEST.json after a part-way run -- the finished "
                "target is on disk and nowhere else")
        else:
            m4 = sorted(t["target"] for t in manifest()["targets"])
            print("   recorded before the failure: %s" % ", ".join(m4))
            if m4 != ["lakritz_gpio"]:
                failures.append(
                    "part-way manifest should list exactly the finished "
                    "target, got %s" % m4)

        r = run_build(["lakritz_langkatze", "sergei_ml1"])
        if r.returncode != 0:
            failures.append("resuming after a part-way run failed")
        m5 = sorted(t["target"] for t in manifest()["targets"])
        print("   after resuming:              %s" % ", ".join(m5))
        if m5 != ["lakritz_gpio", "lakritz_langkatze", "sergei_ml1"]:
            failures.append("resume lost the target that finished before "
                            "the failure: %s" % m5)

        with open(os.path.join(OUT, "README.txt")) as f:
            rt4 = f.read()
        if "zeitlos-lakritz_gpio.img" not in rt4:
            failures.append(
                "README.txt after resuming does not mention the target "
                "that finished before the failure")
        else:
            print("   README.txt describes it too")

        print("\n== session 5: rebuild the sdcard and nothing else ==")
        #
        # Noticing a stale file on the card AFTER shipping is normal,
        # and re-doing eight place-and-routes to fix it is not. --resume
        # skips every target; --rebuild-sdcard is still real work, so
        # the run has to carry on rather than declaring there is nothing
        # to do.
        #
        # What must NOT happen is the shared kernel and ZAR assets being
        # recompiled: they are already here from the run that made the
        # images, and replacing them would publish a kernel that is not
        # the one inside any of them.
        import mkfatimg as _fat

        def _fake_card(root, raw, ark_dir=None, **kw):
            with open(raw, "wb") as f:
                f.write(b"REBUILT-CARD" * 64)
            return [("docs/ask.md", 10)]

        # The update store is made from the same card's files with the
        # ask packs, which a fresh checkout has not built; this test is
        # about which images a card-only run rebuilds, so the store is
        # only recorded. (test_store.py tests the store itself.)
        published = []

        def _fake_store(st_dir, name, **kw):
            published.append(name)

        real_fat, real_zr_fat = _fat.build, ZR.mkfatimg.build
        real_store = ZR._publish_store
        _fat.build = ZR.mkfatimg.build = _fake_card
        ZR._publish_store = _fake_store
        try:
            gz = os.path.join(OUT, "zeitlos.img.gz")
            with open(gz, "wb") as f:
                f.write(b"STALE-CARD")
            m = manifest()
            m["sdcard"] = {"file": "zeitlos.img.gz", "bytes": 10,
                           "files": ["docs/ask.md"]}
            with open(os.path.join(OUT, "MANIFEST.json"), "w") as f:
                json.dump(m, f, indent=2)

            with open(os.path.join(OUT, "zeitlos-kernel.bin"), "rb") as f:
                kernel_before = f.read()
            before = sorted(t["target"] for t in manifest()["targets"])

            # The three built above, so --resume finds nothing left.
            r = run_build(before, extra=["--resume", "--rebuild-sdcard"])
            if r.returncode != 0:
                failures.append("--resume --rebuild-sdcard did not run:\n      "
                                + "\n      ".join(r.stdout.strip().splitlines()[-6:]))
            if "nothing left to build" in r.stdout:
                failures.append(
                    "--resume --rebuild-sdcard bailed out instead of "
                    "rebuilding the card")

            with open(gz, "rb") as f:
                if f.read() == b"STALE-CARD":
                    failures.append("the sdcard image was not rebuilt")
                else:
                    print("   sdcard rebuilt")

            with open(os.path.join(OUT, "zeitlos-kernel.bin"), "rb") as f:
                if f.read() != kernel_before:
                    failures.append(
                        "a card-only run recompiled the kernel -- the "
                        "published kernel would not match the images")
                else:
                    print("   kernel and ZAR assets untouched")

            if published != ["v" + VERSION]:
                failures.append("a card-only run did not publish the update "
                                "store once (%s)" % published)
            else:
                print("   update store published")

            after = sorted(t["target"] for t in manifest()["targets"])
            if after != before:
                failures.append("a card-only run changed the target list: "
                                "%s -> %s" % (before, after))
            else:
                print("   all %d targets still described" % len(after))
        finally:
            _fat.build, ZR.mkfatimg.build = real_fat, real_zr_fat
            ZR._publish_store = real_store

    finally:
        for p in created:
            if os.path.isdir(p):
                shutil.rmtree(p, ignore_errors=True)
            elif os.path.exists(p):
                os.unlink(p)
        selftest.put_back(saved)

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
