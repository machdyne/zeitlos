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
#   1. Full software clean, ONCE for the whole release. sw/ has no
#      per-board object directories -- every app builds its .o files
#      next to its source -- so a clean is how a release guarantees it
#      is not linking something left over from a developer's last
#      build.
#
#      It used to happen per target, because sw/apps/net was built per
#      board: NET_PHY chose one driver, and switching it changed no
#      .c file, so nothing in a dependency graph noticed and the link
#      reused an eth.o compiled for the other one. net now links both
#      drivers and chooses at runtime (sw/apps/net/net_phy.h), so
#      nothing under sw/ varies by board and the kernel, the apps and
#      the core app archive are built exactly once.
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
#   4. Per target: the gateware, then the flash image assembled from
#      that plus the shared kernel, logo and archive.
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
    """(commit, [dirty paths]).

    `git status --porcelain` already omits anything .gitignore covers,
    which is where build output, release artifacts and Python bytecode
    live -- so what comes back is genuinely tracked-or-new source. The
    paths are returned rather than just a boolean because "the tree is
    dirty" without saying which file is an instruction to go run a
    second command, and the answer is usually one file you forgot.
    """
    try:
        out = subprocess.run(["git", "rev-parse", "HEAD"], cwd=root,
                             capture_output=True, text=True)
        commit = out.stdout.strip() or "unknown"
        status = subprocess.run(["git", "status", "--porcelain"], cwd=root,
                                capture_output=True, text=True).stdout
        paths = [l[3:].strip() for l in status.splitlines() if l.strip()]
        return commit, paths
    except OSError:
        return "unknown", []


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
# A domain is a REAL clock if a net name precedes the primitive
# suffix: "$glbnet$CLK_48$TRELLIS_IO_IN" is CLK_48 -- the system clock,
# arriving on a pin rather than from a PLL -- and missing 48MHz there
# is a hard failure. Only a bare "TRELLIS_IO_IN", with no net name at
# all, is nextpnr bucketing stray IO paths under the primitive.
#
# Taking the last $-segment and matching on that treated the two as
# the same thing, which would have let a genuine system-clock failure
# through as an advisory. The whole point of the advisory category is
# that it covers domains nobody asked for; a named clock was asked
# for.
_PSEUDO_SUFFIX = re.compile(r"^(TRELLIS_IO_IN|TRELLIS_IO_OUT|IO_IN|IO_OUT)$",
                            re.I)


def _final_round(entries):
    """The last complete report, split where a clock name repeats."""
    rounds = [[]]
    for e in entries:
        if any(x["clock"] == e["clock"] for x in rounds[-1]):
            rounds.append([])
        rounds[-1].append(e)
    return rounds[-1], len(rounds)


def is_pseudo_domain(name):
    parts = [p for p in name.split("$") if p and p != "glbnet"]
    if not parts:
        return False
    # More than one part means a real net name is in there.
    return len(parts) == 1 and bool(_PSEUDO_SUFFIX.match(parts[0]))


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

def build_software(root, core_apps, dry=False, jobs=None):
    """Kernel, apps and the core app archive. ONCE for the whole release.

    Nothing under sw/ varies by board any more. It used to: sw/apps/net
    was compiled against one NIC driver chosen by NET_PHY, which made
    net.bin -- and therefore the core app archive, and therefore the
    entire software half of a release -- board-specific for the sake of
    one object file. net now links both drivers and picks one at
    runtime from the feature CSR (sw/apps/net/net_phy.h).

    So this runs once, and every target's flash image embeds the same
    kernel.bin and the same apps.zar. Faster, and one less way for two
    targets in a release to differ from each other without anybody
    meaning them to.

    The BIOS is NOT here: it is board-specific (-DBOARD_x, -DFPGA_x)
    and is baked into the bitstream by ecpbram, so it belongs to the
    gateware step.
    """
    arch = spec.derive_arch(root)
    zar = os.path.join(root, "output", "releases", "apps.zar")

    print("\n=== software (shared by every target) ===")
    print("    arch %s   core apps: %s" % (arch, ", ".join(core_apps)))

    mk = ["make", "-C", root, "ARCH=" + arch]
    if jobs:
        mk += ["-j%d" % jobs]

    print("  [1/3] cleaning")
    run(mk + ["clean_os", "clean_apps"], root, dry=dry)

    print("  [2/3] kernel and apps")
    run(mk + ["os"], root, dry=dry)
    run(mk + ["apps"], root, dry=dry)

    print("  [3/3] core app archive")
    zar_cmd = [sys.executable, os.path.join(root, "tools/mkzar.py"), zar]
    for app in core_apps:
        zar_cmd.append("%s=sw/apps/%s/%s.bin" % (app, app, app))
    out = run(zar_cmd, root, dry=dry)
    for line in (out or "").strip().splitlines():
        print("        %s" % line)

    return {"zar": zar,
            "kernel": os.path.join(root, "sw/os/kernel.bin"),
            "arch": arch,
            "core_apps": list(core_apps)}


def build_target(root, target, version, outdir, software, dry=False,
                 allow_timing_fail=False, keep_generated=False,
                 jobs=None, full_image=False, strict_io_timing=False):
    """Gateware and BIOS for one target, then its flash image.

    `software` is what build_software() returned -- the kernel and the
    core app archive, shared by every target in the release.
    """
    lay = layout_mod.load(root)
    arch = software["arch"]
    # The dirty list is not used here -- cmd_build has already decided
    # whether to proceed. Only the commit is wanted, for zspec.vh's
    # provenance header.
    commit, _ = git_commit(root)

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
    print("    board %s   arch %s   nic %s"
          % (target.board, arch, target.nic() or "none"))

    result = {
        "target": target.name,
        "description": target.description,
        "board": target.board,
        "family": target.family,
        "arch": arch,
        "nic": target.nic(),
        "core_apps": software["core_apps"],
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

        # -- 1. the BIOS is the only board-specific software ----------
        #
        # -DBOARD_x and -DFPGA_x, and it is spliced into the bitstream's
        # BRAM by ecpbram, so it is rebuilt per target while the kernel
        # and apps are not. Cleaned first for the same reason the
        # kernel and apps were: sw/ has no per-board object dirs.
        print("  [1/4] BIOS (the one board-specific binary)")
        run(mk + common + ["clean_bios"], root, dry=dry)

        # -- 2. wipe this board's output dir --------------------------
        print("  [2/4] clearing %s" % outsub)
        if not dry and os.path.isdir(boutput):
            shutil.rmtree(boutput)

        # -- 3. gateware ----------------------------------------------
        print("  [3/4] gateware (yosys + nextpnr -- this is the slow one)")
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

        print("  [4/4] flash image")

        if dry:
            return result

        parts = {
            "gateware": os.path.join(boutput, "soc.bit"),
            "logo": os.path.join(root, "sw/data/images/zeitlos_fb.bin"),
            "kernel": software["kernel"],
            "apps": software["zar"],
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

        # The bitstream also ships on its own, for the fourth iteration
        # of a debugging session when rewriting 1.5MB over JTAG to
        # change one thing is the slow part.
        #
        # The core app archive does NOT ship per target any more -- it
        # is byte-identical on every board now that net picks its
        # driver at runtime, so cmd_build writes one zeitlos-apps.zar
        # alongside zeitlos-kernel.bin.
        dst = os.path.join(outdir, stem + "-gateware.bit")
        shutil.copy2(parts["gateware"], dst)
        result["artifacts"]["gateware"] = os.path.basename(dst)

        result["artifacts"]["flash_image"] = os.path.basename(img_path)
        result["image_bytes"] = len(img)
        result["regions"] = [
            {"region": r.key, "offset": r.offset, "used": used,
             "limit": limit, "file": os.path.basename(p.path)}
            for r, p, used, limit in rows
        ]

    return result
