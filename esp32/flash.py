#!/usr/bin/env python3
"""Flash the ESP32 NIC firmware onto the ULX3S, hand held, and prove it took.

Flashing the ESP32 on this board is four things at once: a bitstream that
puts the ESP32 into download mode, an SD card that has to come out because
its DAT0 line is the ESP32's boot strap, two power cycles to do that, and a
bitstream reload afterwards because the FPGA loses its configuration every
time the board loses power. Getting one of them out of order leaves a board
that looks dead, which is why this exists instead of a list in a document.

What it does:

  * checks, before touching anything, that the firmware is real, newer than
    the page baked into it, and that the serial port is free;
  * stops at each physical step and waits, watching the serial device
    appear and disappear so it knows the board really was unplugged;
  * loads the passthru bitstream and runs esptool with the offsets the
    build itself recorded (nic_selftest.py owns both);
  * reloads soc.bit and then checks the desktop came back AND that the
    page being served is the new one.

  usage: flash.py [--build] [--yes] [--host IP] [--baud N]
                  [--port DEV] [--ftdi-serial S] [--soc BITSTREAM]
                  [--force-stale] [--no-verify]

  --build   rebuild the firmware first (needs ESP-IDF; see idf_env())
  --yes     do not ask before starting; the physical steps still pause
"""

from __future__ import annotations

import argparse
import base64
import glob
import hashlib
import os
import socket
import struct
import subprocess
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import nic_selftest  # load_passthru, flash_nic, read_flash_args, default_port

PROJ = os.path.join(HERE, "zeitlos-nic")
BUILD = os.path.join(PROJ, "build")
PAGE = os.path.join(PROJ, "web", "index.html")
SOC = os.path.join(ROOT, "output", "ulx3s", "soc.bit")

# Where a run can be picked up. The names are the two things a person
# does by hand -- take the card out, put it back -- so an interrupted run
# resumes at whichever one just happened.
STAGES = ("start", "flash", "soc", "verify")

# A string that exists only in the viewer page, and only in the version
# that assembles whole frames. Checked inside the firmware image before
# flashing and inside what the board serves afterwards -- the same marker
# at both ends, so a pass really means the bytes travelled.
PAGE_MARKER = "maxFrame"


class Failed(Exception):
    pass


class Paused(Exception):
    """Stopped at a manual step with nobody at the keyboard to answer.

    Not a failure: the board is in a known state and the message says how
    to pick it up again. This is what happens when the script is driven
    from somewhere without a terminal, which is most of the time when
    somebody else is doing the unplugging."""

    def __init__(self, todo: str, resume: str):
        self.todo = todo
        self.resume = resume


_log = None


def say(msg: str = "") -> None:
    print(msg, flush=True)
    if _log:
        _log.write(msg + "\n")
        _log.flush()


def step(n: int, total: int, msg: str) -> None:
    say(f"\n[{n}/{total}] {msg}")


def tee_call(cmd, env=None) -> None:
    """Run a command, show it live, and put it in the log too.

    Subprocess output goes to the terminal's file descriptor, so it walks
    straight past say() and a --log run ended up with the script's own
    lines and none of esptool's. Reading the pipe in chunks rather than
    lines keeps the progress bars moving; only the last state of a line
    rewritten with carriage returns is written to the log, where the
    animation is noise."""
    if _log:
        _log.write("+ " + " ".join(cmd) + "\n")
        _log.flush()
    p = subprocess.Popen(cmd, env=env, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT)
    buf = b""
    while True:
        chunk = os.read(p.stdout.fileno(), 4096)
        if not chunk:
            break
        sys.stdout.buffer.write(chunk)
        sys.stdout.buffer.flush()
        if _log:
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                _log.write(line.split(b"\r")[-1].decode("utf-8", "replace") + "\n")
            _log.flush()
    rc = p.wait()
    if _log and buf:
        _log.write(buf.split(b"\r")[-1].decode("utf-8", "replace") + "\n")
        _log.flush()
    if rc:
        raise subprocess.CalledProcessError(rc, cmd)


def md5(path: str) -> str:
    h = hashlib.md5()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def ask(prompt: str, resume: str) -> None:
    """Stop for something only a person with hands can do.

    The prompt goes through say(), not through input()'s own prompt
    argument: input() writes that to stdout without flushing, so behind a
    pipe (a tee into a log, say) the question would not appear until
    after it had already blocked waiting for the answer.

    With no terminal to ask, this is not an error: say what needs doing
    and how to resume, and stop there."""
    say()
    say("  ---> " + prompt)
    if not sys.stdin.isatty():
        raise Paused(prompt, resume)
    say("       press Enter when done (Ctrl-C to abort): ")
    try:
        input()
    except (EOFError, KeyboardInterrupt):
        raise Failed("aborted at a manual step")


def wait_port(port: str, present: bool, timeout: float = 60.0) -> bool:
    """Wait for the serial device to appear or vanish.

    The board is powered over the same USB cable that carries this device
    node, so its coming and going is the honest signal that the power
    cycle actually happened -- better than trusting a keypress."""
    what = "appear" if present else "disappear"
    t0 = time.time()
    while time.time() - t0 < timeout:
        if os.path.exists(port) == present:
            if present:
                time.sleep(1.5)  # let the FTDI settle before opening it
            return True
        time.sleep(0.3)
    say(f"       (gave up waiting for {port} to {what}; carrying on)")
    return False


def port_busy(port: str) -> str | None:
    try:
        out = subprocess.run(["lsof", port], capture_output=True, text=True).stdout
    except FileNotFoundError:
        return None
    lines = [l for l in out.splitlines()[1:] if l.strip()]
    return lines[0].split()[0] if lines else None


def idf_env() -> dict:
    """The ESP-IDF environment, without export.sh.

    export.sh is the documented way in and usually the right one, but it
    is also fragile: under bash 3.2 (every stock macOS) it aborts while
    generating its completion script -- "Activation script failed ...
    SIGABRT" -- then prints "Done!" anyway and leaves PATH without the
    toolchain, so idf.py is not found a moment after being told all is
    well. idf_tools.py export prints the same variables and does not
    crash, so that is what this uses."""
    idf = os.environ.get("IDF_PATH") or os.path.expanduser("~/esp/esp-idf")
    venv = os.environ.get("IDF_PYTHON_ENV_PATH")
    if not venv:
        found = sorted(glob.glob(os.path.expanduser(
            "~/.espressif/python_env/idf*_env")), reverse=True)
        venv = found[0] if found else ""
    py = os.path.join(venv, "bin", "python") if venv else ""
    if not os.path.isdir(idf):
        raise Failed(f"no ESP-IDF at {idf}; set IDF_PATH")
    if not py or not os.path.isfile(py):
        raise Failed(f"no ESP-IDF python env at {venv or '~/.espressif'}; "
                     "set IDF_PYTHON_ENV_PATH")
    out = subprocess.run(
        [py, os.path.join(idf, "tools", "idf_tools.py"), "export",
         "--format", "key-value"],
        capture_output=True, text=True, check=True).stdout
    env = os.environ.copy()
    env["IDF_PATH"] = idf
    env["IDF_PYTHON_ENV_PATH"] = venv
    for line in out.splitlines():
        if "=" not in line or line.startswith(" "):
            continue
        k, v = line.split("=", 1)
        if k not in ("PATH", "OPENOCD_SCRIPTS", "ESP_ROM_ELF_DIR", "ESP_IDF_VERSION"):
            continue
        env[k] = v.replace("$PATH", os.environ.get("PATH", ""))
    return env


def build() -> None:
    env = idf_env()
    cmd = [os.path.join(env["IDF_PYTHON_ENV_PATH"], "bin", "python"),
           os.path.join(env["IDF_PATH"], "tools", "idf.py"), "-C", PROJ, "build"]
    say("+ " + " ".join(cmd))
    tee_call(cmd, env=env)


def load_soc(ftdi: str | None, bitstream: str) -> None:
    cmd = ["openFPGALoader", "-b", "ulx3s"]
    if ftdi:
        cmd += ["--ftdi-serial", ftdi]
    cmd.append(bitstream)
    say("+ " + " ".join(cmd))
    tee_call(cmd)


def visor_host(arg: str | None) -> str | None:
    """Where to look for the desktop afterwards.

    There is no way to work this out from here: the address is the one
    the AP's DHCP server handed the ESP32, and the board's own copy of it
    (NET.IP, which net writes at the root of the SD card when the link
    comes up) is inside the board. So it is --host, or ZEITLOS_HOST, or
    the run ends after the reload with nothing checked. The console says
    it too, at 1 Mbaud: 'esp_netif_handlers: sta ip:'."""
    return arg or os.environ.get("ZEITLOS_HOST") or None


def fetch(host: str, timeout: float) -> str | None:
    try:
        with urllib.request.urlopen(f"http://{host}/", timeout=timeout) as r:
            return r.read().decode("utf-8", "replace")
    except Exception:
        return None


def wait_visor(host: str, timeout: float = 90.0) -> str | None:
    say(f"       waiting for the desktop at http://{host}/ ...")
    t0 = time.time()
    while time.time() - t0 < timeout:
        page = fetch(host, 3.0)
        if page:
            say(f"       up after {time.time() - t0:.0f}s")
            return page
        time.sleep(2)
    return None


def desktop_alive(host: str, seconds: float = 12.0) -> str:
    """Does the framebuffer still stream? Count the stripes that arrive.

    A websocket client small enough to keep here: the handshake, then
    binary frames of [idx:u8, len:u16le, data...] records (screend.c's
    relay_task). The PackBits payload is not decoded -- this only needs
    to know that all thirty stripe indices turned up, which is the end to
    end proof that net is scanning and the relay is relaying."""
    key = base64.b64encode(os.urandom(16)).decode()
    s = socket.create_connection((host, 80), 5)
    s.sendall((f"GET /ws HTTP/1.1\r\nHost: {host}\r\n"
               f"Upgrade: websocket\r\nConnection: Upgrade\r\n"
               f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n"
               ).encode())
    buf = b""
    seen: set[int] = set()
    t0 = time.time()
    try:
        while b"\r\n\r\n" not in buf:
            chunk = s.recv(4096)
            if not chunk:
                return "no websocket"
            buf += chunk
        if b"101" not in buf.split(b"\r\n", 1)[0]:
            return "websocket refused"
        buf = buf.split(b"\r\n\r\n", 1)[1]
        s.settimeout(1.0)
        while time.time() - t0 < seconds and len(seen) < 30:
            if len(buf) < 2:
                try:
                    chunk = s.recv(8192)
                except socket.timeout:
                    continue
                if not chunk:
                    break
                buf += chunk
                continue
            ln, i = buf[1] & 0x7F, 2
            if ln == 126:
                if len(buf) < 4:
                    buf += s.recv(8192)
                    continue
                ln, i = struct.unpack(">H", buf[2:4])[0], 4
            elif ln == 127:
                if len(buf) < 10:
                    buf += s.recv(8192)
                    continue
                ln, i = struct.unpack(">Q", buf[2:10])[0], 10
            if len(buf) < i + ln:
                try:
                    chunk = s.recv(8192)
                except socket.timeout:
                    continue
                if not chunk:
                    break
                buf += chunk
                continue
            frame, buf = buf[i:i + ln], buf[i + ln:]
            j = 0
            while j + 3 <= len(frame):
                idx = frame[j]
                n = frame[j + 1] | (frame[j + 2] << 8)
                j += 3 + n
                if idx < 30:
                    seen.add(idx)
    finally:
        # A CLEAN close, not just a dropped connection: the ESP32's httpd
        # logs "Failed to read header byte ... closing socket now" for a
        # dropped one, net relays that to the console, and the console is
        # shared with everything else that prints there.
        try:
            s.sendall(b"\x88\x80" + os.urandom(4))
            s.settimeout(0.3)
            s.recv(64)
        except Exception:
            pass
        try:
            s.close()
        except Exception:
            pass
    return f"{len(seen)}/30 stripes"


def preflight(args, verify_only: bool = False) -> str | None:
    """Everything that can be wrong before any hardware is touched.

    With --from verify there is nothing about to be flashed, so none of
    the build checks apply and none of the build has to exist: that run
    is two questions asked of a board that is already up, and it is
    worth having on its own after a board moves network."""
    if verify_only:
        host = visor_host(args.host)
        say("desktop    " + (f"http://{host}/" if host
                             else "unknown (--host or ZEITLOS_HOST to check it)"))
        return host

    for path in (args.soc, nic_selftest.PASSTHRU, PAGE):
        if not os.path.isfile(path):
            raise Failed(f"missing: {path}")

    flags, files = nic_selftest.read_flash_args(BUILD)
    for _, path in files:
        if not os.path.isfile(path):
            raise Failed(f"flash_args names a file that is not there: {path}")

    app = os.path.join(BUILD, "zeitlos-nic.bin")
    if os.path.getmtime(app) < os.path.getmtime(PAGE):
        msg = (f"{os.path.basename(app)} is OLDER than web/index.html, so the "
               "page in it is not the page on disk. Re-run with --build "
               "(or --force-stale to flash it anyway).")
        if not args.force_stale:
            raise Failed(msg)
        say("  WARNING: " + msg)

    with open(app, "rb") as fh:
        blob = fh.read()
    if PAGE_MARKER.encode() not in blob:
        raise Failed(f"{app} does not contain {PAGE_MARKER!r}: this firmware "
                     "does not carry the per-frame viewer page")

    busy = port_busy(args.port)
    if busy:
        raise Failed(f"{args.port} is held by {busy}; close it "
                     "(a console, a monitor) and try again")

    host = visor_host(args.host)
    say("firmware   " + app)
    say(f"           {len(blob)} bytes, md5 {md5(app)}")
    say(f"           carries {PAGE_MARKER!r}: yes")
    say("layout     " + " ".join(flags))
    for off, path in files:
        say(f"           {off:>8}  {os.path.relpath(path, ROOT)}")
    say("passthru   " + os.path.relpath(nic_selftest.PASSTHRU, ROOT))
    say("soc.bit    " + os.path.relpath(args.soc, ROOT))
    say(f"port       {args.port} (free)   baud {args.baud}"
        + (f"   ftdi {args.ftdi_serial}" if args.ftdi_serial else ""))
    say("desktop    " + (f"http://{host}/" if host
                         else "unknown (--host or ZEITLOS_HOST to check it)"))
    return host


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", action="store_true",
                    help="rebuild the firmware before flashing")
    ap.add_argument("--yes", action="store_true", help="do not ask before starting")
    ap.add_argument("--host", help="where the desktop should answer "
                                   "(default: $ZEITLOS_HOST; without it the "
                                   "result is not checked)")
    ap.add_argument("--port", default=nic_selftest.DEFAULT_PORT,
                    help="board serial device (default: the only one found)")
    ap.add_argument("--ftdi-serial",
                    help="only needed with more than one board attached")
    ap.add_argument("--baud", type=int, default=460800)
    ap.add_argument("--soc", default=SOC,
                    help="the Zeitlos bitstream to put back (default: "
                         "output/ulx3s/soc.bit)")
    ap.add_argument("--force-stale", action="store_true",
                    help="flash even if the firmware predates the page")
    ap.add_argument("--no-verify", action="store_true")
    ap.add_argument("--log", help="also write everything said here to this file")
    ap.add_argument("--from", dest="start", default="start", choices=STAGES,
                    help="pick up a run that stopped: 'flash' if the card is "
                         "already out and the board is powered, 'soc' if it is "
                         "back in, 'verify' to only check the result")
    args = ap.parse_args()
    if not args.port:
        say("no board serial device found; say --port")
        return 2

    global _log
    if args.log:
        _log = open(args.log, "a")
        # esptool and openFPGALoader print to the terminal, not through
        # say(); route them through tee_call so the log is the whole run
        nic_selftest.RUNNER = tee_call
        say(f"# esp32 flash {time.strftime('%Y-%m-%d %H:%M:%S')} "
            f"{' '.join(sys.argv[1:])}")

    total = 8
    try:
        stage = STAGES.index(args.start)
        verify_only = stage == STAGES.index("verify")
        step(1, total, "checking the board" if verify_only
             else "checking what is about to be flashed")
        if args.build:
            build()
        host = preflight(args, verify_only)
        if stage:
            say(f"\n(resuming at '{args.start}')")

        if not args.yes and not stage:
            say()
            say("This power-cycles the board twice and rewrites the ESP32's flash.")
            say("Go ahead? [y/N] ")
            try:
                if input().strip().lower() not in ("y", "yes"):
                    say("nothing done")
                    return 1
            except (EOFError, KeyboardInterrupt):
                say("\nnothing done")
                return 1

        if stage < STAGES.index("flash"):
            step(2, total, "taking the SD card out")
            say("  The card's DAT0 is GPIO2 on the ESP32, which is a boot strap:")
            say("  with the card in, the ESP32 will not enter download mode. And")
            say("  the card cannot be pulled live, so the board has to be unplugged.")
            ask("UNPLUG the ULX3S from USB.", "--from start")
            wait_port(args.port, present=False, timeout=90)
            ask("Take the SD card OUT, then plug the board back in.", "--from flash")
            if not wait_port(args.port, present=True, timeout=90):
                raise Failed(f"{args.port} never came back; is the board plugged in?")
            say("       serial back")

        if stage < STAGES.index("soc"):
            step(3, total, "passthru bitstream (puts the ESP32 in download mode)")
            nic_selftest.load_passthru(args.ftdi_serial)
            time.sleep(1.5)

            step(4, total, "writing the ESP32 flash")
            try:
                nic_selftest.flash_nic(args.port, baud=args.baud, build_dir=BUILD)
            except subprocess.CalledProcessError:
                if args.baud <= 115200:
                    raise
                say("\n  esptool failed at %d baud; retrying at 115200" % args.baud)
                nic_selftest.flash_nic(args.port, baud=115200, build_dir=BUILD)

            step(5, total, "putting the SD card back")
            ask("UNPLUG the ULX3S again.", "--from soc")
            wait_port(args.port, present=False, timeout=90)
            ask("Put the SD card BACK IN, then plug the board back in.", "--from soc")
            if not wait_port(args.port, present=True, timeout=90):
                raise Failed(f"{args.port} never came back; is the board plugged in?")

        if stage < STAGES.index("verify"):
            step(6, total, "reloading soc.bit (the FPGA lost it with the power)")
            load_soc(args.ftdi_serial, args.soc)

        if args.no_verify or host is None:
            say("\nflashed; verification skipped"
                + ("" if args.no_verify else " (no --host and no ZEITLOS_HOST)"))
            return 0

        step(7, total, "waiting for the desktop")
        page = wait_visor(host)
        if page is None:
            raise Failed(
                f"no answer from http://{host}/ .\n"
                "  The ESP32 is held in reset by the FPGA, so this usually means\n"
                "  the bitstream did not take, or the board joined a different\n"
                "  network. Read the console at 1 Mbaud (DTR/RTS low) and look\n"
                "  for 'esp_netif_handlers: sta ip:'.")

        step(8, total, "is it serving the new page?")
        if PAGE_MARKER not in page:
            raise Failed(f"the desktop answers but the page has no {PAGE_MARKER!r}: "
                         "the ESP32 is still running the old firmware")
        say(f"       page carries {PAGE_MARKER!r}: yes ({len(page)} bytes)")
        say("       framebuffer: " + desktop_alive(host))

        say("\nDone. The desktop is at http://%s/" % host)
        return 0

    except Paused as e:
        say()
        say("Stopped here because there is nobody to press Enter.")
        say("Do that, then run:")
        say(f"  python3 {os.path.relpath(os.path.abspath(__file__), os.getcwd())} "
            f"{e.resume} --yes" + (f" --log {args.log}" if args.log else ""))
        return 3
    except Failed as e:
        say("\nFAILED: %s" % e)
        return 2
    except subprocess.CalledProcessError as e:
        say("\nFAILED: command exited %d" % e.returncode)
        return 2


if __name__ == "__main__":
    sys.exit(main())
