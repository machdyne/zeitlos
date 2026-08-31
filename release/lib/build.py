#
# Zeitlos release tooling -- the build half.
#
# Deliberately drives the TOP-LEVEL MAKEFILE rather than reimplementing
# what it does. A release built by a second, parallel set of build rules
# is a release that can differ from what `make BOARD=x flash` produces
# on a developer's machine, and that difference would only ever be
# discovered from a bug report. So everything here is a `make` call with
# variables set; nothing here knows how to run yosys.
#
# Order per target, and the reasons for it:
#
#   1. Full software clean. NOT optional and NOT an optimisation to
#      remove later. sw/ has no per-board object directories -- every
#      app builds its .o files next to its source -- so the objects
#      left behind by the previous target are still sitting there.
#      sw/apps/net/Makefile's header describes what that costs: switch
#      NET_PHY and the .c files are unchanged, so nothing in a
#      dependency graph notices, and the link happily reuses an eth.o
#      compiled for the other driver. The .net_phy_selected stamp file
#      guards that case specifically, but a clean guards all of them.
#
#   2. Wipe output/<board>. lakritz_uart and lakritz_langkatze share
#      output/lakritz, because the Makefile keys that directory off
#      BOARD and not off the target. If the second build fails at
#      place-and-route, the first build's soc.bit is still sitting
#      there looking perfectly valid, and it would be picked up and
#      shipped under the wrong name. Removing it makes that failure
#      loud.
#
#   3. Gateware (which pulls in the BIOS -- the Makefile's `zeitlos`
#      target does bios, soc, os and apps in one chain, and the BIOS
#      has to be built before `soc` because ecpbram splices bios.hex
#      into the bitstream's BRAM).
#
#   4. Kernel and apps, with the target's derived software config.
#
#   5. ZAR, then the assembled image.
#
# Between 3 and 5 the pnr log is read for timing. See check_timing().
#

import os
import re
import shutil
import subprocess
import sys

import gen
import layout as layout_mod
import mkflashimg
import spec


class BuildError(Exception):
    pass


def run(cmd, cwd, env=None, dry=False, echo=True):
    if echo:
        print("    $ %s" % " ".join(cmd))
    if dry:
        return ""
    e = dict(os.environ)
    if env:
        e.update(env)
    r = subprocess.run(cmd, cwd=cwd, env=e, text=True,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if r.returncode != 0:
        # Show the tail rather than the whole thing -- a failed yosys or
        # nextpnr run is thousands of lines and the useful part is at
        # the bottom.
        tail = "\n".join(r.stdout.splitlines()[-40:])
        raise BuildError("command failed: %s\n\n...\n%s"
                         % (" ".join(cmd), tail))
    return r.stdout


# ---------------------------------------------------------------------
# Version
#
# sw/common/zversion.h's comment says the version is hand-maintained
# and deliberately carries no build date or git hash, so that "is the
# flashed image the one I just built?" stays answerable by comparing
# bytes. That is a good reason and this tool does not override it.
#
# So the version is VERIFIED, not generated. `zrelease build v0.0.3`
# against a header still saying 0.0.2 stops before it builds anything,
# rather than shipping an image whose `info` app reports the previous
# release. `--bump` edits the header, as a deliberate act you then
# commit.
# ---------------------------------------------------------------------

VERSION_H = "sw/common/zversion.h"


def read_version(root):
    path = os.path.join(root, VERSION_H)
    if not os.path.exists(path):
        raise BuildError("%s: not found" % VERSION_H)
    with open(path) as f:
        m = re.search(r'#define\s+Z_OS_VERSION\s+"([^"]*)"', f.read())
    if not m:
        raise BuildError("%s: no Z_OS_VERSION" % VERSION_H)
    return m.group(1)


def bump_version(root, version):
    path = os.path.join(root, VERSION_H)
    with open(path) as f:
        text = f.read()
    new = re.sub(r'(#define\s+Z_OS_VERSION\s+")[^"]*(")',
                 r"\g<1>%s\g<2>" % version, text)
    with open(path, "w") as f:
        f.write(new)


def normalise_version(v):
    """v0.0.3 and 0.0.3 are the same release; the tag keeps the v."""
    return v[1:] if v.startswith("v") else v


def git_commit(root):
    try:
        out = subprocess.run(["git", "rev-parse", "HEAD"], cwd=root,
                             capture_output=True, text=True)
        commit = out.stdout.strip() or "unknown"
        dirty = subprocess.run(["git", "status", "--porcelain"], cwd=root,
                               capture_output=True, text=True).stdout.strip()
        return commit, bool(dirty)
    except OSError:
        return "unknown", False


# ---------------------------------------------------------------------
# Timing
#
# The top-level Makefile already greps pnr.log for "FAIL at" and prints
# a warning, and its comment explains why: a bitstream that missed
# timing still programs and still half-works, which is far harder to
# debug than one that refused to build.
#
# A warning is the right response during development. For a release it
# is not -- nobody reads scrollback from a build they did not run, and
# an intermittently-misbehaving image is the single worst thing to put
# behind a download link. So this fails the build, with --allow-timing-fail
# as the deliberate override.
# ---------------------------------------------------------------------

# nextpnr reports Fmax MORE THAN ONCE per run -- once after placement,
# from an estimate, and again after routing with real delays. Reading
# the whole log and keeping every match concatenates the rounds, which
# shows each clock twice with different numbers and, worse, fails a
# release on a placement estimate that routing then fixed.
#
# The final report is the only one that describes the bitstream that
# was actually written, so it is the only one that counts.
#
# Split on repetition rather than on a header string: a clock name
# appearing for the second time means a new round has started. That
# needs no assumption about nextpnr's wording, and copes with one
# round, two, or some future number.
_FMAX = re.compile(r"Max frequency for clock\s+'([^']+)':\s+"
                   r"([\d.]+)\s*MHz\s+\(([A-Z]+) at ([\d.]+)\s*MHz\)")

# Domains that are not a clock anybody wrote. nextpnr names a domain
# after whatever drives it, and paths that begin or end at an IO pin
# get bucketed under the IO primitive's own name rather than a net from
# the design. Those are reported and recorded, but they do not by
# themselves block a release: there is no PLL to retune and no
# constraint in the .lpf that asked for them, so a "FAIL" here is
# usually nextpnr assuming a default target for a path that has no
# meaningful frequency at all.
#
# NOT ignored silently. They are shown in the summary, kept in
# MANIFEST.json, and named in the build output, so a real problem
# hiding behind one is visible rather than swallowed. Set
# strict_io_timing=True to gate on them too.
_PSEUDO_DOMAIN = re.compile(r"^(TRELLIS_IO|IO_IN|IO_OUT|\$?PACKER)", re.I)


def _final_round(entries):
    """The last complete report, split where a clock name repeats."""
    rounds = [[]]
    for e in entries:
        if any(x["clock"] == e["clock"] for x in rounds[-1]):
            rounds.append([])
        rounds[-1].append(e)
    return rounds[-1], len(rounds)


def is_pseudo_domain(name):
    return bool(_PSEUDO_DOMAIN.match(name.rsplit("$", 1)[-1]))


def check_timing(pnr_log, strict_io_timing=False):
    if not os.path.exists(pnr_log):
        return {"clocks": [], "failed": False, "utilisation": {},
                "rounds": 0, "advisory": []}

    with open(pnr_log, errors="replace") as f:
        text = f.read()

    seen = [{"clock": m.group(1), "achieved_mhz": float(m.group(2)),
             "result": m.group(3), "target_mhz": float(m.group(4))}
            for m in _FMAX.finditer(text)]
    clocks, rounds = _final_round(seen)

    # Utilisation is printed once per run today, but take the last
    # occurrence for the same reason as above.
    util = {}
    for m in re.finditer(r"^\s*Info:\s+(\w+):\s+(\d+)/\s*(\d+)\s+(\d+)%",
                         text, re.M):
        util[m.group(1)] = {"used": int(m.group(2)), "total": int(m.group(3)),
                            "percent": int(m.group(4))}

    hard, advisory = [], []
    for c in clocks:
        if c["result"] != "FAIL":
            continue
        if is_pseudo_domain(c["clock"]) and not strict_io_timing:
            advisory.append(c)
        else:
            hard.append(c)

    return {"clocks": clocks,
            "rounds": rounds,
            "failed": bool(hard),
            "failing": hard,
            "advisory": advisory,
            "utilisation": util}


def timing_summary(t):
    if not t["clocks"]:
        return "no timing data"
    parts = []
    for c in t["clocks"]:
        name = c["clock"].rsplit("$", 1)[-1]
        tag = c["result"]
        if c["result"] == "FAIL" and c in t.get("advisory", []):
            tag = "FAIL (advisory)"
        parts.append("%s %.1f MHz %s" % (name, c["achieved_mhz"], tag))
    out = "; ".join(parts)
    if t.get("rounds", 1) > 1:
        out += "   [final of %d reports]" % t["rounds"]
    return out


# ---------------------------------------------------------------------
# The build
# ---------------------------------------------------------------------

def build_target(root, target, version, outdir, dry=False,
                 allow_timing_fail=False, keep_generated=False,
                 jobs=None, full_image=False, strict_io_timing=False):
    lay = layout_mod.load(root)
    arch = spec.derive_arch(root)
    sw = target.derive_sw()
    commit, dirty = git_commit(root)

    board_lc = target.board.lower()

    # NOT output/<board>. A release build wipes this directory before
    # each target -- it has to, because two targets on one board share
    # it and a failed second build would otherwise leave the first
    # one's soc.bit sitting there looking perfectly valid -- and that
    # wipe must not take a developer's working bitstream with it. The
    # top-level Makefile's OUTDIR exists for this.
    outsub = os.path.join("output", "releases", board_lc)
    boutput = os.path.join(root, outsub)

    print("\n=== %s (%s) ===" % (target.name, target.description))
    print("    board %s   arch %s   net %s"
          % (target.board, arch, sw["NET_PHY"] or "none"))

    result = {
        "target": target.name,
        "description": target.description,
        "board": target.board,
        "family": target.family,
        "arch": arch,
        "net_phy": sw["NET_PHY"],
        "core_apps": target.core_app_list(),
        "defines": {k: v for k, v in target.defines.items()},
        "flash_cmd": target.flash_cmd,
        "notes": list(target.notes),
        "artifacts": {},
    }

    # Refuse before spending twenty minutes in nextpnr to be told the
    # same thing less clearly.
    cov = gen.check_port_coverage(root, target, spec)
    if cov:
        raise BuildError("%s: %s" % (target.name, "\n  ".join(cov)))

    with gen.Generated(root, keep=keep_generated) as g:
        # -- generated inputs ------------------------------------------
        g.write("rtl/zspec.vh", gen.zspec_vh(target, version, commit))

        lpf_rel, lpf_text = gen.merged_lpf(root, target)
        lpf_name = target.lpf
        if lpf_rel:
            g.write(lpf_rel, lpf_text)
            lpf_name = os.path.basename(lpf_rel)

        mk = ["make", "-C", root]
        common = ["BOARD=" + target.board,
                  "EXTRA_DEFINES=-DZSPEC",
                  "OUTDIR=" + outsub,
                  "ARCH=" + arch]
        if lpf_name:
            common.append("LPF=" + lpf_name)
        if jobs:
            mk += ["-j%d" % jobs]

        # -- 1. clean --------------------------------------------------
        print("  [1/6] cleaning software objects")
        run(mk + common + ["clean_os", "clean_bios", "clean_apps"],
            root, dry=dry)

        # -- 2. wipe this board's output dir ---------------------------
        print("  [2/6] clearing %s" % outsub)
        if not dry and os.path.isdir(boutput):
            shutil.rmtree(boutput)

        # -- 3. gateware + bios ----------------------------------------
        #
        # `zeitlos` is bios -> place-and-route -> soc -> os -> apps.
        # The software half is rebuilt below with the target's config;
        # this is about the bitstream and the BIOS baked into it.
        print("  [3/6] gateware (yosys + nextpnr -- this is the slow one)")
        run(mk + common + ["zeitlos_pico", "bios", "soc"], root, dry=dry)

        pnr_log = os.path.join(boutput, "pnr.log")
        timing = check_timing(pnr_log, strict_io_timing=strict_io_timing)
        result["timing"] = timing
        print("        %s" % timing_summary(timing))
        for c in timing.get("advisory", []):
            print("        note: %s missed its target (%.1f of %.1f MHz). "
                  "That domain is an IO"
                  % (c["clock"].rsplit("$", 1)[-1], c["achieved_mhz"],
                     c["target_mhz"]))
            print("        primitive rather than a clock from the design, "
                  "so it is recorded but not gated.")
            print("        Use --strict-io-timing to treat it as a "
                  "failure.")
        if timing["utilisation"]:
            u = timing["utilisation"]
            key = "TRELLIS_COMB" if "TRELLIS_COMB" in u else sorted(u)[0]
            print("        %s %d/%d (%d%%)"
                  % (key, u[key]["used"], u[key]["total"], u[key]["percent"]))

        if timing["failed"] and not allow_timing_fail:
            names = ", ".join("%s (%.1f of %.1f MHz)"
                              % (c["clock"].rsplit("$", 1)[-1],
                                 c["achieved_mhz"], c["target_mhz"])
                              for c in timing["failing"])
            raise BuildError(
                "%s missed timing on %s.\n\n"
                "  %s\n\n"
                "  A bitstream that missed timing programs fine and then "
                "misbehaves intermittently, which is the worst thing to\n"
                "  put behind a download link. Look at the critical path "
                "with\n"
                "    make path BOARD=%s OUTDIR=%s\n"
                "  (OUTDIR because release builds do not share a directory "
                "with your development ones)\n"
                "  and either fix it or, if you have decided the margin is "
                "acceptable, rerun with --allow-timing-fail."
                % (target.name, names, timing_summary(timing), board_lc,
                   outsub))

        # -- 4. kernel -------------------------------------------------
        print("  [4/6] kernel")
        run(mk + common + ["os"], root, dry=dry)

        # -- 5. apps ---------------------------------------------------
        #
        # NET_PHY is passed as a command-line variable, which is what
        # makes it win: sw/apps/net/Makefile uses `NET_PHY ?= RMII`, and
        # a command-line assignment overrides a ?= and propagates
        # through the `cd net && make` sub-make via MAKEFLAGS.
        print("  [5/6] apps%s"
              % (" (NET_PHY=%s)" % sw["NET_PHY"] if sw["NET_PHY"] else ""))
        app_vars = list(common)
        if sw["NET_PHY"]:
            app_vars.append("NET_PHY=" + sw["NET_PHY"])
        run(mk + app_vars + ["apps"], root, dry=dry)

        # -- 6. ZAR + image --------------------------------------------
        print("  [6/6] core app archive and flash image")
        zar_path = os.path.join(boutput, "apps.zar")
        zar_cmd = [sys.executable, os.path.join(root, "tools/mkzar.py"),
                   zar_path]
        for app in target.core_app_list():
            zar_cmd.append("%s=sw/apps/%s/%s.bin" % (app, app, app))
        out = run(zar_cmd, root, dry=dry)
        if out:
            for line in out.strip().splitlines():
                print("        %s" % line)

        if dry:
            return result

        parts = {
            "gateware": os.path.join(boutput, "soc.bit"),
            "logo": os.path.join(root, "sw/data/images/zeitlos_fb.bin"),
            "kernel": os.path.join(root, "sw/os/kernel.bin"),
            "apps": zar_path,
        }
        img, rows = mkflashimg.build(lay, parts, full=full_image)

        # -- Asset names carry the TARGET but not the VERSION -----------
        #
        # "zeitlos-lakritz_uart.img" rather than
        # "zeitlos-v0.0.3-lakritz_uart.img", so that
        #
        #   .../releases/latest/download/zeitlos-lakritz_uart.img
        #
        # is a permanent URL that always resolves to the newest release.
        # The project README can then print a command that stays
        # correct, which a versioned name would make impossible -- and
        # that README already relies on exactly this for
        # zeitlos.img.gz.
        #
        # The version is not lost: it is the release tag, it is in
        # MANIFEST.json, it is in README.txt next to the files, and the
        # info app reports it from the running system. A filename is the
        # one place it is least useful and most costly.
        os.makedirs(outdir, exist_ok=True)
        stem = "zeitlos-%s" % target.name
        img_path = os.path.join(outdir, stem + ".img")
        with open(img_path, "wb") as f:
            f.write(img)
        print()
        print(mkflashimg.describe(rows, len(img), lay["flash_size"]))
        print()

        # The individual pieces ship too. The combined image is what a
        # new owner wants; these are what you want on the fourth
        # iteration of a debugging session, when rewriting 1.5MB over
        # JTAG to change one app is the slow part.
        for key, src in (("gateware", parts["gateware"]),
                         ("apps", parts["apps"])):
            ext = "-gateware.bit" if key == "gateware" else "-apps.zar"
            dst = os.path.join(outdir, stem + ext)
            shutil.copy2(src, dst)
            result["artifacts"][key] = os.path.basename(dst)

        result["artifacts"]["flash_image"] = os.path.basename(img_path)
        result["image_bytes"] = len(img)
        result["regions"] = [
            {"region": r.key, "offset": r.offset, "used": used,
             "limit": limit, "file": os.path.basename(p.path)}
            for r, p, used, limit in rows
        ]

    return result
