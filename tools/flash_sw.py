#!/usr/bin/env python3
"""Flash zeitlos_sw.bin with openFPGALoader without erasing the key/value store.

Why: zeitlos_sw.bin (kernel + ZAR) starts at FLASH_BASE and ends inside the
last 64 KB block of the 1 MiB Zeitlos region (+0x0F0000..+0x0FFFFF).
openFPGALoader erases that entire 64 KB block -- wiping the key/value store
at +0x0FE000 -- both on flashes without 4 KB subsector erase and on flashes
that do support it (such as W25Q128), due to a bug in openFPGALoader's
SPIFlash::sectors_erase() that falls back to 64 KB block erase (0xD8) on the
second 4 KB step of a partial tail block (trabucayre/openFPGALoader#765,
fix proposed in trabucayre/openFPGALoader#766).

What this tool does instead:
  1. Reads the key/value store twice with --dump-flash and refuses to write
     if the two reads do not match byte for byte.
  2. Composes a full 1 MiB region image: zeitlos_sw.bin, 0xFF padding up to
     Z_FLASH_KV_OFF, and the store as read.
  3. Writes the composed image at FLASH_BASE with --verify.
  4. Reads the key/value store back and confirms its digest is unchanged.

Offsets and sizes are read from release/lib/layout.py (derived from
sw/common/zsoc.h) rather than duplicated here. Store contents are never
printed -- only their MD5 digest, since the store holds private keys.
"""

import argparse
import hashlib
import os
import shlex
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "release", "lib"))
import layout  # noqa: E402


def md5_bytes(data):
    return hashlib.md5(data).hexdigest()


def build_dump_cmd(flash_tokens, kv_addr, kv_size, out_path):
    """Turn a board's FLASH command into the matching --dump-flash command."""
    cmd = [t for t in flash_tokens if t not in ("-f", "--write-flash", "--verify")]
    cmd.extend(["--dump-flash", "-o", str(kv_addr), "--file-size", str(kv_size), out_path])
    return cmd


def build_write_cmd(flash_tokens, base, img_path):
    """Turn a board's FLASH command into a verified write at `base`."""
    cmd = list(flash_tokens)
    if "-f" not in cmd and "--write-flash" not in cmd:
        cmd.append("-f")
    if "--verify" not in cmd:
        cmd.append("--verify")
    cmd.extend(["-o", str(base), img_path])
    return cmd


def compose_image(sw_data, kv_data, kv_off, kv_size, region_size):
    """Compose a full region image: sw_data + 0xFF padding + kv_data."""
    if len(sw_data) > kv_off:
        raise ValueError(
            "image is %d bytes; it reaches the key/value store at +0x%x"
            % (len(sw_data), kv_off)
        )
    if len(kv_data) != kv_size:
        raise ValueError(
            "key/value store dump is %d bytes, expected %d"
            % (len(kv_data), kv_size)
        )
    if kv_off + kv_size != region_size:
        raise ValueError(
            "layout mismatch: kv_off (0x%x) + kv_size (0x%x) != region_size (0x%x)"
            % (kv_off, kv_size, region_size)
        )
    return sw_data + (b"\xff" * (kv_off - len(sw_data))) + kv_data


def run_cmd(cmd):
    p = subprocess.run(cmd)
    return p.returncode


def dump_kv(flash_tokens, kv_addr, kv_size, out_path):
    cmd = build_dump_cmd(flash_tokens, kv_addr, kv_size, out_path)
    rc = run_cmd(cmd)
    if rc != 0:
        raise RuntimeError("dump failed (exit %d): nothing written" % rc)
    if not os.path.exists(out_path):
        raise RuntimeError("dump produced no output file: nothing written")
    with open(out_path, "rb") as f:
        data = f.read()
    if len(data) != kv_size:
        raise RuntimeError(
            "dump is %d bytes, expected %d: nothing written"
            % (len(data), kv_size)
        )
    return data


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Flash zeitlos_sw.bin with openFPGALoader while preserving the key/value store."
    )
    ap.add_argument("image", help="path to zeitlos_sw.bin")
    ap.add_argument("--base", required=True, help="FLASH_BASE (hex or decimal, e.g. 0x100000)")
    ap.add_argument("--flash-cmd", help="openFPGALoader command (from Makefile FLASH)")
    ap.add_argument("--dry-run", action="store_true",
                    help="compose the 1 MiB image and print planned commands without writing")
    ap.add_argument("--kv-in", help="use an existing 8 KB store dump instead of reading flash (with --dry-run)")
    ap.add_argument("--kv-empty", action="store_true",
                    help="use an all-0xFF 8 KB store instead of reading flash (with --dry-run)")
    ap.add_argument("--out", help="write the composed 1 MiB image to this path")
    args = ap.parse_args(argv)

    base = int(args.base, 0)
    lay = layout.load(ROOT)
    kv_off = lay["offsets"]["kv"]
    kv_size = lay["offsets"]["kv_size"]
    region_size = lay["region_size"]
    kv_addr = base + kv_off

    if not os.path.exists(args.image):
        print("flash_sw: %s: not found" % args.image, file=sys.stderr)
        return 1
    with open(args.image, "rb") as f:
        sw_data = f.read()

    if len(sw_data) > kv_off:
        print(
            "flash_sw: %s is %d bytes; it reaches the key/value store at +0x%x"
            % (args.image, len(sw_data), kv_off),
            file=sys.stderr,
        )
        return 1

    if args.kv_in and args.kv_empty:
        print("flash_sw: --kv-in and --kv-empty are mutually exclusive", file=sys.stderr)
        return 1
    if (args.kv_in or args.kv_empty) and not args.dry_run:
        print("flash_sw: --kv-in / --kv-empty require --dry-run", file=sys.stderr)
        return 1

    flash_tokens = shlex.split(args.flash_cmd) if args.flash_cmd else []
    if not args.dry_run and not flash_tokens:
        print("flash_sw: --flash-cmd is required", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory(prefix="zeitlos-flash-sw-") as tmp:
        if args.kv_empty:
            kv_before = b"\xff" * kv_size
        elif args.kv_in:
            with open(args.kv_in, "rb") as f:
                kv_before = f.read()
        else:
            try:
                kv1 = dump_kv(flash_tokens, kv_addr, kv_size, os.path.join(tmp, "kv-before-1.bin"))
                kv2 = dump_kv(flash_tokens, kv_addr, kv_size, os.path.join(tmp, "kv-before-2.bin"))
            except RuntimeError as e:
                print("flash_sw: %s" % e, file=sys.stderr)
                return 1
            if kv1 != kv2:
                print(
                    "flash_sw: the two reads of the key/value store at 0x%06x differ "
                    "(%s vs %s): refusing to write"
                    % (kv_addr, md5_bytes(kv1), md5_bytes(kv2)),
                    file=sys.stderr,
                )
                return 1
            kv_before = kv1

        empty_note = " (empty, all 0xFF)" if kv_before == b"\xff" * kv_size else ""
        print(
            "flash_sw: kv store at 0x%06x (%d bytes): md5 %s%s"
            % (kv_addr, kv_size, md5_bytes(kv_before), empty_note),
            flush=True,
        )

        try:
            composed = compose_image(sw_data, kv_before, kv_off, kv_size, region_size)
        except ValueError as e:
            print("flash_sw: %s" % e, file=sys.stderr)
            return 1

        composed_path = args.out or os.path.join(tmp, "sw-with-kv.bin")
        with open(composed_path, "wb") as f:
            f.write(composed)
        print(
            "flash_sw: composed %d bytes at 0x%06x (image %d bytes, pad %d bytes, kv %d bytes), md5 %s"
            % (len(composed), base, len(sw_data), kv_off - len(sw_data), kv_size, md5_bytes(composed)),
            flush=True,
        )

        if args.dry_run:
            if flash_tokens:
                print("flash_sw: dry-run dump:  %s" % " ".join(
                    build_dump_cmd(flash_tokens, kv_addr, kv_size, "<kv.bin>")))
                print("flash_sw: dry-run write: %s" % " ".join(
                    build_write_cmd(flash_tokens, base, composed_path)))
            return 0

        write_cmd = build_write_cmd(flash_tokens, base, composed_path)
        rc = run_cmd(write_cmd)
        if rc != 0:
            print("flash_sw: write failed (exit %d)" % rc, file=sys.stderr)
            return rc

        try:
            kv_after = dump_kv(flash_tokens, kv_addr, kv_size, os.path.join(tmp, "kv-after.bin"))
        except RuntimeError as e:
            print("flash_sw: post-write %s" % e, file=sys.stderr)
            return 1

        if kv_after != kv_before:
            print(
                "flash_sw: key/value store changed after write! before %s, after %s"
                % (md5_bytes(kv_before), md5_bytes(kv_after)),
                file=sys.stderr,
            )
            return 1

        print(
            "flash_sw: kv store verified after flash: md5 %s (unchanged)"
            % md5_bytes(kv_after),
            flush=True,
        )
        return 0


if __name__ == "__main__":
    sys.exit(main())
