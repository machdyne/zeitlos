#!/usr/bin/env python3
"""Flash zeitlos-nic onto a Kabellos PMOD, and prove it took.

Kabellos is much simpler to flash than the ULX3S (flash.py): its
ESP32-C3 has USB built in, wired to the module's micro-USB socket, and
esptool can reset that chip into its bootloader over USB alone. No
passthru bitstream, no sdcard to pull, no power cycles. Plug a cable
into the Kabellos micro-USB while it sits in the Lakritz PMOD port and
run this.

  python3 esp32/kabellos_flash.py --build      build, then flash
  python3 esp32/kabellos_flash.py              flash the existing build
  python3 esp32/kabellos_flash.py --image zeitlos-nic-kabellos.bin
                                               flash a release image
                                               (needs only esptool)
  python3 esp32/kabellos_flash.py --info       ask the running firmware
                                               what it is; flash nothing

What it checks before writing anything:

  * the image is for an ESP32-C3 and is zeitlos-nic -- an ULX3S image
    would be refused by the chip's ROM anyway, but saying so here is
    clearer than an esptool error;
  * a local build is newer than every source it was built from
    (--force-stale to flash it anyway);
  * there is exactly one C3 USB device (Espressif 303a:1001). A
    Lakritz's own USB console is a CDC-ACM device too, so "the only
    ttyACM" is not a safe guess; the USB ID is.

And afterwards: it waits for the C3 to come back on USB (it
re-enumerates on every reset), sends `info` to the firmware's CLI, and
compares the ELF hash the firmware reports with the one inside the
image it just wrote. A match means the chip is running exactly that
image, not merely that esptool said "done".

THE ONE THING THAT CATCHES PEOPLE: the C3 is only on USB while it is
running, and the FPGA holds it in reset (PMOD pin 1, EN) until Zeitlos
starts `net`. So boot to the desktop first. docs/kabellos.md, "Flashing",
has the details and the alternatives.

  usage: kabellos_flash.py [--build] [--image FILE] [--port DEV]
                           [--info] [--force-stale] [--no-verify]
                           [--idf PATH]
"""

from __future__ import annotations

import argparse
import glob
import os
import re
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import nic_build      # BOARDS, merged_image, build, find_idf
import nic_selftest   # run, esptool_python, read_flash_args

PROJ = nic_build.PROJ
BUILD = nic_build.BOARDS["kabellos"][1]
CHIP = "esp32c3"

ESPRESSIF_VID = 0x303A
USB_JTAG_PID = 0x1001

APP_DESC_MAGIC = 0xABCD5432
CHIP_ID = {0: "esp32", 2: "esp32s2", 5: "esp32c3", 9: "esp32s3",
           12: "esp32c2", 13: "esp32c6", 16: "esp32h2"}

# Everything a build is made from. An image older than any of these was
# built from something else.
SOURCES = ("main", "web", "CMakeLists.txt", "sdkconfig.defaults",
           "sdkconfig.defaults.esp32c3")


# ---- the image ---------------------------------------------------------

def image_identity(path: str) -> tuple[str, str]:
    """(chip, elf hash prefix) of a zeitlos-nic image.

    Works on both the bare app (build-kabellos/zeitlos-nic.bin) and the
    merged image a release ships, by finding the app descriptor: magic
    0xABCD5432 followed by the project name. It sits 32 bytes into the
    app image (24-byte image header, 8-byte segment header), and the
    image header carries the chip id at offset 12."""
    with open(path, "rb") as fh:
        data = fh.read()
    magic = struct.pack("<I", APP_DESC_MAGIC)
    i = data.find(magic)
    while i >= 0:
        name = data[i + 48:i + 80].split(b"\0", 1)[0]
        if name == b"zeitlos-nic":
            break
        i = data.find(magic, i + 4)
    if i < 32:
        raise SystemExit("%s: not a zeitlos-nic image (no app descriptor)"
                         % path)
    hdr = i - 32
    if data[hdr] != 0xE9:
        raise SystemExit("%s: app descriptor without an image header" % path)
    chip_id = struct.unpack_from("<H", data, hdr + 12)[0]
    chip = CHIP_ID.get(chip_id, "chip id %d" % chip_id)
    elf = data[i + 144:i + 176].hex()[:16]
    return chip, elf


def newest_source() -> tuple[float, str]:
    newest, which = 0.0, ""
    for s in SOURCES:
        p = os.path.join(PROJ, s)
        paths = [p] if os.path.isfile(p) else [
            os.path.join(d, f) for d, _, fs in os.walk(p) for f in fs]
        for f in paths:
            m = os.path.getmtime(f)
            if m > newest:
                newest, which = m, f
    return newest, which


# ---- the USB device ----------------------------------------------------

def c3_ports() -> list[str]:
    """Every ESP32-C3/S3/C6 USB Serial/JTAG device attached."""
    try:
        from serial.tools import list_ports
    except ImportError:
        raise SystemExit("pyserial is missing: pip install pyserial esptool "
                         "(or run from the ESP-IDF environment)")
    return sorted(p.device for p in list_ports.comports()
                  if p.vid == ESPRESSIF_VID and p.pid == USB_JTAG_PID)


def find_port(explicit: str | None) -> str:
    if explicit:
        return explicit
    ports = c3_ports()
    if len(ports) == 1:
        return ports[0]
    if len(ports) > 1:
        raise SystemExit("more than one ESP32-C3 on USB (%s); say --port"
                         % ", ".join(ports))
    acm = sorted(glob.glob("/dev/ttyACM*") + glob.glob("/dev/cu.usbmodem*"))
    msg = ["no Kabellos (ESP32-C3, USB 303a:1001) found on USB.", "",
           "  * Is the micro-USB cable in the Kabellos socket, and is it a",
           "    data cable rather than a charging one?",
           "  * Is the C3 running? Zeitlos holds it in reset (PMOD pin 1,",
           "    EN) until `net` starts: boot a lakritz_kabellos image to",
           "    the desktop first. Under any other bitstream the PMOD",
           "    pins are not driven for Kabellos -- see docs/kabellos.md,",
           "    \"Flashing\"."]
    if acm:
        msg += ["", "  (serial devices that are NOT a C3: %s -- one is" % ", ".join(acm),
                "   probably the Lakritz's own USB console)"]
    raise SystemExit("\n".join(msg))


def wait_for_port(port: str, timeout: float) -> str | None:
    """The C3 drops off USB on every reset and comes back, sometimes
    under another name. Wait for one to be present again."""
    deadline = time.time() + timeout
    time.sleep(0.5)
    while time.time() < deadline:
        ports = c3_ports()
        if port in ports:
            return port
        if len(ports) == 1:
            return ports[0]
        time.sleep(0.25)
    return None


def query_info(port: str, timeout: float = 15.0) -> dict | None:
    """Send `info` to the firmware CLI and parse the ZTEST info line.

    DTR and RTS are set false BEFORE the port opens: on the C3's USB
    Serial/JTAG those two lines are the chip's reset and boot-mode
    controls, and pyserial asserts both on open by default. Toggling
    them is how esptool resets the chip; doing it here by accident would
    put it in its bootloader, where nothing answers `info`."""
    import serial
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = 115200
    ser.timeout = 0.2
    ser.dtr = False
    ser.rts = False
    try:
        ser.open()
    except serial.SerialException as e:
        print("cannot open %s: %s" % (port, e), file=sys.stderr)
        return None
    buf = b""
    deadline = time.time() + timeout
    next_ask = 0.0
    try:
        while time.time() < deadline:
            if time.time() >= next_ask:
                ser.write(b"\ninfo\n")
                ser.flush()
                next_ask = time.time() + 1.0
            buf += ser.read(4096)
            m = re.search(rb"ZTEST info ([^\r\n]*)\r?\n", buf)
            if m:
                line = m.group(1).decode("latin1")
                info = dict(re.findall(r"(\w+)=(\S+)", line))
                info["_line"] = line
                return info
    finally:
        ser.close()
    tail = buf.decode("latin1", "replace").strip().splitlines()[-5:]
    if tail:
        print("no `info` answer; last output:\n  " + "\n  ".join(tail),
              file=sys.stderr)
    return None


# ---- main --------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", action="store_true",
                    help="build the Kabellos firmware first (needs ESP-IDF)")
    ap.add_argument("--image", default=None,
                    help="flash this merged image at 0x0 instead of the local "
                         "build (a release's zeitlos-nic-kabellos.bin)")
    ap.add_argument("--port", default=None,
                    help="the C3's serial device (default: the only one)")
    ap.add_argument("--baud", type=int, default=460800,
                    help="ignored by USB Serial/JTAG, kept for adapters")
    ap.add_argument("--info", action="store_true",
                    help="only ask the running firmware what it is")
    ap.add_argument("--force-stale", action="store_true",
                    help="flash a build older than its sources")
    ap.add_argument("--no-verify", action="store_true",
                    help="do not check what the chip runs afterwards")
    ap.add_argument("--idf", default=None, help="ESP-IDF checkout (--build)")
    args = ap.parse_args()

    if args.info:
        port = find_port(args.port)
        info = query_info(port)
        if not info:
            print("FAIL: the firmware on %s did not answer `info` (older "
                  "firmware has no such command; or it is not zeitlos-nic)"
                  % port, file=sys.stderr)
            return 1
        print(info["_line"])
        return 0

    if args.build and args.image:
        ap.error("--build and --image are alternatives")

    # 1. what to write
    if args.build:
        nic_build.build("kabellos", nic_build.find_idf(args.idf), False)
    if args.image:
        image = os.path.abspath(args.image)
        writes = [("0x0", image)]
        flags: list[str] = []
        ident_file = image
    else:
        app = os.path.join(BUILD, "zeitlos-nic.bin")
        if not os.path.exists(os.path.join(BUILD, "flash_args")):
            raise SystemExit("no Kabellos build in %s: run with --build, or "
                             "python3 esp32/nic_build.py kabellos"
                             % os.path.relpath(BUILD, os.getcwd()))
        flags, writes = nic_selftest.read_flash_args(BUILD)
        for _, f in writes:
            if not os.path.exists(f):
                raise SystemExit("flash_args names %s, which is missing; rebuild" % f)
        newest, which = newest_source()
        if os.path.getmtime(app) < newest and not args.force_stale:
            raise SystemExit("the build is older than %s; rebuild (--build) "
                             "or --force-stale" % os.path.relpath(which, HERE))
        ident_file = app

    chip, elf = image_identity(ident_file)
    if chip != CHIP:
        raise SystemExit("%s is an image for %s, not %s -- %s"
                         % (os.path.basename(ident_file), chip, CHIP,
                            "that is the ULX3S build; Kabellos needs "
                            "zeitlos-nic-kabellos.bin" if chip == "esp32"
                            else "not a Kabellos image"))
    print("image   %s  (%s, elf %s)" % (os.path.relpath(ident_file, os.getcwd()),
                                        chip, elf))

    # 2. where to write it
    port = find_port(args.port)
    print("port    %s" % port)

    # 3. write. --before default_reset over USB Serial/JTAG resets the C3
    # into its ROM bootloader through the USB peripheral itself, which
    # does not depend on the BOOT strap (PMOD pin 2) the FPGA holds high.
    cmd = [nic_selftest.esptool_python(), "-m", "esptool",
           "--chip", CHIP, "-p", port, "-b", str(args.baud),
           "--before", "default_reset", "--after", "hard_reset",
           "write_flash"] + flags
    for off, f in writes:
        cmd += [off, f]
    nic_selftest.run(cmd)

    if args.no_verify:
        print("flashed (not verified)")
        return 0

    # 4. is it running what we wrote?
    print("waiting for the C3 to come back on USB ...", flush=True)
    port2 = wait_for_port(port, 15.0)
    if not port2:
        print("FAIL: the C3 did not reappear on USB within 15 s. If Zeitlos "
              "restarted meanwhile it may be holding it in reset; check "
              "with --info once the desktop is up.", file=sys.stderr)
        return 1
    info = query_info(port2)
    if not info:
        print("FAIL: flashed, but the firmware did not answer `info`",
              file=sys.stderr)
        return 1
    print("running %s" % info["_line"])
    if info.get("board") != "kabellos" or info.get("elf") != elf:
        print("FAIL: the chip reports board=%s elf=%s; the image is elf %s"
              % (info.get("board"), info.get("elf"), elf), file=sys.stderr)
        return 1
    print("OK: Kabellos runs the image just written. Zeitlos's `net` sees "
          "the reboot as a new HELLO and re-sends the WiFi credentials by "
          "itself.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
