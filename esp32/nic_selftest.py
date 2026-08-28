#!/usr/bin/env python3
"""Drive zeitlos-nic over UART0 (passthru). No SD, no Zeitlos.

Usage:
  python3 esp32/nic_selftest.py --ssid MyAP --psk 'secret'
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time

DEFAULT_PORT = "/dev/cu.usbserial-K00443"
PASSTHRU = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "passthru", "ulx3s_85f_passthru.bit"
)
NIC_BUILD = os.path.join(os.path.dirname(os.path.abspath(__file__)), "zeitlos-nic", "build")
OPENFPGA = "openFPGALoader"


def run(cmd: list[str]) -> None:
    print("+", " ".join(cmd), flush=True)
    # python.org 3.10.4 (the ESP-IDF venv base) aborts at exit on recent
    # macOS ("pointer being freed was not allocated" in Py_FinalizeEx);
    # PYTHONMALLOC=malloc sidesteps the pymalloc/libmalloc mismatch.
    env = os.environ.copy()
    env.setdefault("PYTHONMALLOC", "malloc")
    subprocess.check_call(cmd, env=env)


def load_passthru(serial: str) -> None:
    env = os.environ.copy()
    oss = "/Users/carl/Apps/oss-cad-suite/bin"
    if os.path.isdir(oss):
        env["PATH"] = oss + os.pathsep + env.get("PATH", "")
    cmd = [OPENFPGA, "-b", "ulx3s", "--ftdi-serial", serial, PASSTHRU]
    print("+", " ".join(cmd), flush=True)
    subprocess.check_call(cmd, env=env)


def flash_nic(port: str) -> None:
    py = os.path.expanduser("~/.espressif/python_env/idf5.4_py3.10_env/bin/python")
    if not os.path.isfile(py):
        py = sys.executable
    run(
        [
            py,
            "-m",
            "esptool",
            "--chip",
            "esp32",
            "-p",
            port,
            "-b",
            "115200",
            "--before",
            "default_reset",
            "--after",
            "hard_reset",
            "write_flash",
            "--flash_mode",
            "dio",
            "--flash_size",
            "4MB",
            "--flash_freq",
            "40m",
            "0x1000",
            os.path.join(NIC_BUILD, "bootloader", "bootloader.bin"),
            "0x8000",
            os.path.join(NIC_BUILD, "partition_table", "partition-table.bin"),
            "0x10000",
            os.path.join(NIC_BUILD, "zeitlos-nic.bin"),
        ]
    )


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
    ap.add_argument("--port", default=DEFAULT_PORT)
    ap.add_argument("--ftdi-serial", default="K00443")
    ap.add_argument("--no-passthru", action="store_true")
    ap.add_argument("--flash", action="store_true")
    ap.add_argument("--ping-only", action="store_true")
    args = ap.parse_args()

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
