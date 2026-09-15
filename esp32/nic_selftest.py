#!/usr/bin/env python3
"""Drive zeitlos-nic over UART0 (passthru). No SD, no Zeitlos.

Usage:
  python3 esp32/nic_selftest.py --ssid MyAP --psk 'secret'
"""

from __future__ import annotations

import argparse
import glob
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PASSTHRU = os.path.join(HERE, "passthru", "ulx3s_85f_passthru.bit")
NIC_BUILD = os.path.join(HERE, "zeitlos-nic", "build")
OPENFPGA = "openFPGALoader"

# How every subprocess here is started. A caller that wants to see AND
# record what they print (esp32/flash.py --log) swaps in something that
# tees; nothing below calls subprocess directly, so one swap covers
# esptool and openFPGALoader both.
RUNNER = subprocess.check_call


def default_port() -> str | None:
    """The board's USB serial device, if there is exactly one candidate.

    The ULX3S shows up as an FT231X: /dev/cu.usbserial-<FTDI serial> on
    macOS, /dev/ttyUSB* on Linux. Guessing is only safe when there is
    one; with two boards (or a USB-serial adapter also plugged in) say
    --port, because picking the wrong one flashes somebody else's chip."""
    found = sorted(glob.glob("/dev/cu.usbserial-*") + glob.glob("/dev/ttyUSB*"))
    return found[0] if len(found) == 1 else None


DEFAULT_PORT = default_port()


def run(cmd: list[str]) -> None:
    print("+", " ".join(cmd), flush=True)
    # python.org 3.10.4 (the ESP-IDF venv base) aborts at exit on recent
    # macOS ("pointer being freed was not allocated" in Py_FinalizeEx);
    # PYTHONMALLOC=malloc sidesteps the pymalloc/libmalloc mismatch.
    env = os.environ.copy()
    env.setdefault("PYTHONMALLOC", "malloc")
    RUNNER(cmd, env=env)


def load_passthru(serial: str | None = None) -> None:
    """Configure the FPGA so the ESP32's UART0 reaches the USB serial.

    openFPGALoader has to be on PATH. --ftdi-serial is only needed with
    more than one board attached; without it openFPGALoader takes the
    one it finds."""
    cmd = [OPENFPGA, "-b", "ulx3s"]
    if serial:
        cmd += ["--ftdi-serial", serial]
    cmd.append(PASSTHRU)
    print("+", " ".join(cmd), flush=True)
    RUNNER(cmd)


def esptool_python() -> str:
    """The interpreter that has esptool. The ESP-IDF venv always does."""
    venv = os.environ.get("IDF_PYTHON_ENV_PATH")
    cands = [os.path.join(venv, "bin", "python")] if venv else []
    cands += sorted(glob.glob(os.path.expanduser(
        "~/.espressif/python_env/idf*_env/bin/python")), reverse=True)
    for py in cands:
        if os.path.isfile(py):
            return py
    return sys.executable


def read_flash_args(build_dir: str = NIC_BUILD):
    """What idf.py says to write and where, from build/flash_args.

    Read rather than hardcoded because the offsets belong to the build:
    a partition table that grows moves the app, and a copy of the numbers
    here would keep flashing the old layout without saying so. Returns
    (flags, [(offset, absolute path), ...])."""
    path = os.path.join(build_dir, "flash_args")
    flags: list[str] = []
    files: list[tuple[str, str]] = []
    with open(path) as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            if line.startswith("--"):
                flags = line.split()
                continue
            off, name = line.split(None, 1)
            files.append((off, os.path.join(build_dir, name)))
    if not files:
        raise RuntimeError(f"{path} lists nothing to flash")
    return flags, files


def flash_nic(port: str, baud: int = 115200, build_dir: str = NIC_BUILD) -> None:
    flags, files = read_flash_args(build_dir)
    cmd = [
        esptool_python(), "-m", "esptool",
        "--chip", "esp32",
        "-p", port,
        "-b", str(baud),
        "--before", "default_reset",
        "--after", "hard_reset",
        "write_flash",
    ] + flags
    for off, path in files:
        cmd += [off, path]
    run(cmd)


def reset_esp32(ser) -> None:
    ser.dtr = False
    ser.rts = True
    time.sleep(0.05)
    ser.dtr = True
    ser.rts = False
    time.sleep(0.05)
    ser.dtr = False
    ser.rts = False


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--ssid", required=True)
    ap.add_argument("--psk", required=True)
    ap.add_argument("--port", default=DEFAULT_PORT,
                    help="board serial device (default: the only one found)")
    ap.add_argument("--ftdi-serial", default=None,
                    help="only needed with more than one board attached")
    ap.add_argument("--no-passthru", action="store_true")
    ap.add_argument("--flash", action="store_true")
    ap.add_argument("--ping-only", action="store_true")
    args = ap.parse_args()
    if not args.port:
        print("no board serial device found; say --port", file=sys.stderr)
        return 2

    if not args.no_passthru:
        load_passthru(args.ftdi_serial)
        time.sleep(1.2)
    if args.flash:
        flash_nic(args.port)
        time.sleep(0.5)

    import serial

    ser = serial.Serial(args.port, 115200, timeout=0.2)
    try:
        ser.dtr = False
        ser.rts = False
    except Exception:
        pass
    reset_esp32(ser)

    buf = b""
    deadline = time.time() + 15
    ready = False
    sys.stdout.write("--- esp32 ---\n")
    sys.stdout.flush()
    while time.time() < deadline:
        chunk = ser.read(4096)
        if chunk:
            sys.stdout.buffer.write(chunk)
            sys.stdout.buffer.flush()
            buf += chunk
            if b"ZTEST ready" in buf:
                ready = True
                break
    if not ready:
        print("FAIL: no ZTEST ready (is zeitlos-nic flashed? passthru loaded?)", file=sys.stderr)
        ser.close()
        return 2

    time.sleep(0.3)
    if args.ping_only:
        cmd = b"ping 8.8.8.8\n"
    else:
        # psk may contain spaces; firmware takes the rest of the line
        cmd = f"test {args.ssid} {args.psk}\n".encode("utf-8")
    ser.write(cmd)
    ser.flush()
    print(">>> test <ssid> <psk>", flush=True)

    deadline = time.time() + 45
    result = None
    while time.time() < deadline:
        chunk = ser.read(4096)
        if not chunk:
            continue
        sys.stdout.buffer.write(chunk)
        sys.stdout.buffer.flush()
        buf += chunk
        text = buf.decode("latin1", "replace")
        if "ZTEST result=PASS" in text:
            result = "PASS"
            break
        if "ZTEST result=FAIL" in text:
            result = "FAIL"
            break

    extra = time.time() + 1.5
    while time.time() < extra:
        chunk = ser.read(4096)
        if chunk:
            sys.stdout.buffer.write(chunk)
            sys.stdout.buffer.flush()
    ser.close()

    if result == "PASS":
        print("--- PASS ---")
        return 0
    print("--- FAIL ---", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
