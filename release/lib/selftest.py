#!/usr/bin/env python3
#
# Exercises the release pipeline without an FPGA toolchain.
#
# Everything from the generated zspec.vh through image assembly,
# manifest, notes and checksums is real code on real data; only the
# `make` calls are stubbed, and they are replaced by something that
# writes plausible artifacts (a bitstream of a believable size, a
# kernel, app binaries) so the assembler and the validators have
# something to be right or wrong about.
#
# Run from the repository root:  python3 release/lib/selftest.py
#

import json
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)

import build as build_mod          # noqa: E402
import layout as layout_mod        # noqa: E402
import mkflashimg                  # noqa: E402
import notes as notes_mod          # noqa: E402
import spec                        # noqa: E402

FAKE_PNR = """\
Info: Device utilisation:
Info:           TRELLIS_COMB: 18211/24288    74%
Info:            TRELLIS_FF:   4102/24288    16%
Info:          TRELLIS_RAMW:    204/ 3036     6%
Info: Max frequency for clock '$glbnet$clk_48': 63.41 MHz (PASS at 48.00 MHz)
Info: Max frequency for clock '$glbnet$vid_clk': 138.02 MHz (PASS at 125.00 MHz)
"""

FAKE_PNR_FAIL = FAKE_PNR.replace("63.41 MHz (PASS at 48.00 MHz)",
                                 "41.02 MHz (FAIL at 48.00 MHz)")


def fake_run(pnr_text):
    """Replacement for build.run that writes what a real build would."""
    def _run(cmd, cwd, env=None, dry=False, echo=True):
        joined = " ".join(cmd)
        if "mkzar.py" in joined:
            # Let the real mkzar.py run -- it is part of what we test.
            return build_mod.__dict__["_real_run"](cmd, cwd, env, dry, False)
        board = next((c.split("=", 1)[1] for c in cmd
                      if c.startswith("BOARD=")), "lakritz").lower()
        out = os.path.join(ROOT, "output", board)
        if "zeitlos_pico" in cmd or "soc" in cmd:
            os.makedirs(out, exist_ok=True)
            with open(os.path.join(out, "soc.bit"), "wb") as f:
                f.write(os.urandom(412 * 1024))
            with open(os.path.join(out, "pnr.log"), "w") as f:
                f.write(pnr_text)
        if "os" in cmd:
            with open(os.path.join(ROOT, "sw/os/kernel.bin"), "wb") as f:
                f.write(os.urandom(118 * 1024))
        if "apps" in cmd:
            for app in ("wm", "net", "repl", "term"):
                p = os.path.join(ROOT, "sw/apps", app, app + ".bin")
                os.makedirs(os.path.dirname(p), exist_ok=True)
                with open(p, "wb") as f:
                    f.write(b"ZEXE" + os.urandom(60 * 1024))
        return ""
    return _run


def cleanup(created):
    for p in created:
        if os.path.isdir(p):
            shutil.rmtree(p, ignore_errors=True)
        elif os.path.exists(p):
            os.unlink(p)


def main():
    os.chdir(ROOT)
    lay = layout_mod.load(ROOT)
    version = "0.0.2-selftest"
    out = os.path.join(ROOT, "release/dist", version)
    created = [out, os.path.join(ROOT, "output")]
    for app in ("wm", "net", "repl", "term"):
        created.append(os.path.join(ROOT, "sw/apps", app, app + ".bin"))
    created.append(os.path.join(ROOT, "sw/os/kernel.bin"))

    build_mod._real_run = build_mod.run
    failures = []

    try:
        # --- 1. timing failure must stop the build --------------------
        print("== a target that missed timing is refused ==")
        build_mod.run = fake_run(FAKE_PNR_FAIL)
        t = spec.load_target(ROOT, "lakritz_uart")
        try:
            build_mod.build_target(ROOT, t, version, out)
            failures.append("a FAIL-at timing report did not stop the build")
            print("   NOT REFUSED -- bug")
        except build_mod.BuildError as e:
            assert "missed timing" in str(e)
            print("   refused, as it should be")

        print("\n   ... and --allow-timing-fail overrides it")
        r = build_mod.build_target(ROOT, t, version, out,
                                   allow_timing_fail=True)
        assert r["timing"]["failed"] is True
        print("   built, with timing recorded as failed in the manifest")

        # --- 2. normal builds ----------------------------------------
        print("\n== building every target ==")
        build_mod.run = fake_run(FAKE_PNR)
        results = []
        for name in ("lakritz_uart", "mozart_ml1", "sergei_ml1"):
            t = spec.load_target(ROOT, name)
            results.append(build_mod.build_target(ROOT, t, version, out))

        # --- 3. the images really do contain what they claim ----------
        print("\n== image contents ==")
        L = {r.key: r for r in lay["regions"]}
        for res in results:
            img_path = os.path.join(out, res["artifacts"]["flash_image"])
            with open(img_path, "rb") as f:
                img = f.read()
            checks = [
                ("ZAR magic at the core-app offset",
                 img[L["apps"].offset:L["apps"].offset + 4] == b"ZAR1"),
                ("logo at its offset",
                 img[L["logo"].offset:L["logo"].offset + 4] != b"\xff" * 4),
                ("kernel at its offset",
                 img[L["kernel"].offset:L["kernel"].offset + 4]
                 != b"\xff" * 4),
                ("gap between logo and kernel left erased",
                 set(img[L["logo"].offset + lay["logo_bytes"]:
                         L["kernel"].offset]) == {0xFF}),
                ("trimmed, not padded to 2MB",
                 len(img) < lay["flash_size"]),
            ]
            bad = [n for n, ok in checks if not ok]
            print("   %-42s %s" % (res["artifacts"]["flash_image"],
                                   "ok" if not bad else "FAILED: %s" % bad))
            failures.extend(bad)

        # --- 4. net follows the hardware, not a setting ---------------
        print("\n== the net app follows the hardware ==")
        for res in results:
            has_net = "net" in res["core_apps"]
            expect = res["net_phy"] is not None
            ok = has_net == expect
            print("   %-16s net_phy=%-9s net in ZAR=%-5s  %s"
                  % (res["target"], res["net_phy"], has_net,
                     "ok" if ok else "MISMATCH"))
            if not ok:
                failures.append("%s: net/PHY mismatch" % res["target"])

        # --- 5. notes and manifest -----------------------------------
        print("\n== notes and manifest ==")
        sdcard = {"file": "zeitlos.img.gz",
                  "bytes": 1204423, "files": ["files", "text", "read"]}
        man = notes_mod.manifest(version, "deadbeef" * 5, False, results,
                                 sdcard, lay, "abc123")
        notes_mod.write(os.path.join(out, "MANIFEST.json"), man)
        text = notes_mod.notes(version, "deadbeef" * 5, results, sdcard, lay)
        notes_mod.write(os.path.join(out, "NOTES.md"), text)
        rt = notes_mod.asset_readme(version, "deadbeef" * 5, results, sdcard,
                                    lay)
        notes_mod.write(os.path.join(out, "README.txt"), rt)
        for must in ("zeitlos-mozart_ml1.img", "RMII", "FLASH LAYOUT",
                     "zeitlos.img.gz"):
            if must not in rt:
                failures.append("README.txt is missing %r" % must)
        print("   README.txt describes every asset")
        print("\n---- README.txt ----\n")
        print(rt)

        reloaded = json.load(open(os.path.join(out, "MANIFEST.json")))
        assert len(reloaded["targets"]) == len(results)
        assert reloaded["targets"][0]["defines"]["MEM"] == "32"
        print("   MANIFEST.json round-trips, %d targets, defines recorded"
              % len(reloaded["targets"]))

        for must in ("openFPGALoader", "## Flashing", "## Flash layout",
                     "0x140000"):
            if must not in text:
                failures.append("NOTES.md is missing %r" % must)
        print("   NOTES.md contains the flash commands and the layout")

        print("\n---- NOTES.md ----\n")
        print(text)

    finally:
        build_mod.run = build_mod._real_run
        cleanup(created)

    print("\n==================")
    if failures:
        print("%d FAILURE(S):" % len(failures))
        for f in failures:
            print("  - %s" % f)
        return 1
    print("all self-tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
