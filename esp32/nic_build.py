#!/usr/bin/env python3
"""Build zeitlos-nic for the ULX3S, for Kabellos, or both.

One source tree (esp32/zeitlos-nic), two chips. The board follows from
the ESP-IDF target (main/Kconfig.projbuild), so a build is "this target
in that directory":

  board     IDF target   build directory                 sdkconfig
  ulx3s     esp32        esp32/zeitlos-nic/build/        zeitlos-nic/sdkconfig
  kabellos  esp32c3      esp32/zeitlos-nic/build-kabellos/  build-kabellos/sdkconfig

The ULX3S keeps the paths `idf.py build` has always used, so flash.py,
nic_selftest.py and anyone's muscle memory are unaffected. Kabellos gets
its own directory AND its own sdkconfig: `idf.py set-target` rewrites
the sdkconfig it is pointed at, and two boards sharing one would mean
every switch is a full clean and a chance to flash the wrong chip.

Each build also writes a single merged image for flashing at 0x0:

  build/zeitlos-nic-ulx3s.bin
  build-kabellos/zeitlos-nic-kabellos.bin

That is the file a release ships (docs/releases.md) and the one
`kabellos_flash.py --image` writes, for people without ESP-IDF.

  usage: nic_build.py [ulx3s|kabellos|all] [--clean] [--idf PATH]

ESP-IDF v5.4 is needed (docs/esp32link.md, "Installing ESP-IDF"). If
`idf.py` is already on PATH (an exported IDF shell) that is used;
otherwise the IDF at --idf PATH, $IDF_PATH or ~/esp/esp-idf, in that
order, with its environment taken from idf_tools.py rather than
export.sh -- see idf_env().
"""

from __future__ import annotations

import argparse
import glob
import os
import re
import shlex
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.join(HERE, "zeitlos-nic")

# board -> (IDF target, build dir, sdkconfig path or None for the default)
BOARDS = {
    "ulx3s": ("esp32", os.path.join(PROJ, "build"), None),
    "kabellos": ("esp32c3", os.path.join(PROJ, "build-kabellos"),
                 os.path.join(PROJ, "build-kabellos", "sdkconfig")),
}


def merged_image(board: str) -> str:
    """Where the single-file image for `board` lands."""
    return os.path.join(BOARDS[board][1], "zeitlos-nic-%s.bin" % board)


def find_idf(explicit: str | None) -> str | None:
    """An ESP-IDF checkout to take the environment from, or None if
    idf.py is already on PATH (we are inside an exported shell)."""
    if not explicit and shutil.which("idf.py"):
        return None
    for cand in (explicit, os.environ.get("IDF_PATH"),
                 os.path.expanduser("~/esp/esp-idf")):
        if cand and os.path.isfile(os.path.join(cand, "tools", "idf.py")):
            return cand
    raise SystemExit("ESP-IDF not found: run from an exported IDF shell, "
                     "set IDF_PATH, or pass --idf PATH (ESP-IDF v5.4; "
                     "docs/esp32link.md, \"Installing ESP-IDF\")")


def idf_version(idf_dir: str) -> str:
    """"5.4" for a v5.4 checkout, from the IDF's own version.cmake."""
    v = {}
    with open(os.path.join(idf_dir, "tools", "cmake", "version.cmake")) as fh:
        for line in fh:
            m = re.match(r"\s*set\(IDF_VERSION_(MAJOR|MINOR)\s+(\d+)\)", line)
            if m:
                v[m.group(1)] = m.group(2)
    return "%s.%s" % (v.get("MAJOR", "?"), v.get("MINOR", "?"))


def idf_env(idf_dir: str) -> tuple[dict, str]:
    """(environment, python) for running idf.py from `idf_dir`.

    NOT by sourcing export.sh, for the reason esp32/flash.py's idf_env()
    gives: under bash 3.2 -- every stock macOS -- export.sh aborts while
    generating its completion script, prints "Done!" anyway, and leaves
    PATH without the toolchain. `idf_tools.py export` prints the same
    variables and does not crash.

    The Python environment is the one install.sh made for THIS checkout's
    version (~/.espressif/python_env/idf5.4_py*_env). Taking the newest
    one instead would run a v5.4 tree under a v5.5 environment on any
    machine that has both, which fails in ways that do not mention
    versions at all."""
    ver = idf_version(idf_dir)
    venv = os.environ.get("IDF_PYTHON_ENV_PATH")
    if not venv:
        found = sorted(glob.glob(os.path.expanduser(
            "~/.espressif/python_env/idf%s_*_env" % ver)), reverse=True)
        venv = found[0] if found else ""
    py = os.path.join(venv, "bin", "python") if venv else ""
    if not py or not os.path.isfile(py):
        raise SystemExit("no ESP-IDF %s python environment in "
                         "~/.espressif/python_env: run %s/install.sh "
                         "esp32,esp32c3 (docs/esp32link.md, \"Installing "
                         "ESP-IDF\"), or set IDF_PYTHON_ENV_PATH"
                         % (ver, idf_dir))
    out = subprocess.run(
        [py, os.path.join(idf_dir, "tools", "idf_tools.py"), "export",
         "--format", "key-value"],
        capture_output=True, text=True, check=True).stdout
    env = os.environ.copy()
    env["IDF_PATH"] = idf_dir
    env["IDF_PYTHON_ENV_PATH"] = venv
    for line in out.splitlines():
        if "=" not in line or line.startswith(" "):
            continue
        k, v = line.split("=", 1)
        if k not in ("PATH", "OPENOCD_SCRIPTS", "ESP_ROM_ELF_DIR",
                     "ESP_IDF_VERSION"):
            continue
        env[k] = v.replace("$PATH", os.environ.get("PATH", ""))
    return env, py


def idf(idf_dir: str | None, args: list[str]) -> None:
    """Run idf.py in the project directory, inside the IDF environment."""
    if idf_dir:
        env, py = idf_env(idf_dir)
        cmd = [py, os.path.join(idf_dir, "tools", "idf.py")] + args
    else:
        env, cmd = os.environ.copy(), ["idf.py"] + args
    env.setdefault("PYTHONMALLOC", "malloc")	# see nic_selftest.run()
    print("+ idf.py " + " ".join(shlex.quote(a) for a in args), flush=True)
    subprocess.check_call(cmd, cwd=PROJ, env=env)


def configured_target(sdkconfig: str) -> str | None:
    try:
        with open(sdkconfig) as fh:
            for line in fh:
                if line.startswith("CONFIG_IDF_TARGET="):
                    return line.split("=", 1)[1].strip().strip('"')
    except OSError:
        pass
    return None


def build(board: str, idf_dir: str | None, clean: bool) -> str:
    target, bdir, sdk = BOARDS[board]
    common = ["-B", bdir]
    if sdk:
        common += ["-DSDKCONFIG=" + sdk]
    sdk_path = sdk or os.path.join(PROJ, "sdkconfig")

    if clean and os.path.isdir(bdir):
        print("removing", os.path.relpath(bdir, HERE))
        shutil.rmtree(bdir)
        if sdk is None and os.path.exists(sdk_path):
            os.remove(sdk_path)

    # set-target only when needed: it is a full clean, and for the ULX3S
    # it rewrites the sdkconfig a person may have tuned with menuconfig.
    have = configured_target(sdk_path)
    if have != target:
        if have:
            print("%s: sdkconfig is for %s, switching to %s"
                  % (board, have, target))
        idf(idf_dir, common + ["set-target", target])
    idf(idf_dir, common + ["build"])

    # One file at 0x0, from the offsets the build itself recorded.
    out = merged_image(board)
    idf(idf_dir, common + ["merge-bin", "-o", out])
    print("%s: %s (%d bytes)" % (board, os.path.relpath(out, HERE),
                                 os.path.getsize(out)))
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("board", nargs="?", default="all",
                    choices=sorted(BOARDS) + ["all"])
    ap.add_argument("--clean", action="store_true",
                    help="delete the build directory first")
    ap.add_argument("--idf", default=None, help="ESP-IDF checkout to use")
    args = ap.parse_args()

    idf_dir = find_idf(args.idf)
    boards = sorted(BOARDS) if args.board == "all" else [args.board]
    for b in boards:
        build(b, idf_dir, args.clean)
    return 0


if __name__ == "__main__":
    sys.exit(main())
