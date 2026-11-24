#!/usr/bin/env python3
"""Host tests for tools/flash_sw.py using a stub openFPGALoader."""

import hashlib
import os
import stat
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FLASH_SW = os.path.join(ROOT, "tools", "flash_sw.py")
sys.path.insert(0, os.path.join(ROOT, "release", "lib"))
import layout  # noqa: E402


STUB_LOADER = """#!/usr/bin/env python3
import json, os, sys

state_dir = os.environ["FAKE_OFL_DIR"]
log_path = os.path.join(state_dir, "calls.jsonl")
args = sys.argv[1:]
with open(log_path, "a") as f:
    f.write(json.dumps(args) + "\\n")

mode_file = os.path.join(state_dir, "mode.txt")
mode = open(mode_file).read().strip() if os.path.exists(mode_file) else "ok"
flash_path = os.path.join(state_dir, "flash.bin")
flash = bytearray(open(flash_path, "rb").read())

if "--dump-flash" in args:
    off = int(args[args.index("-o") + 1], 0)
    size = int(args[args.index("--file-size") + 1], 0)
    out = args[-1]
    cnt_path = os.path.join(state_dir, "dump_count.txt")
    cnt = int(open(cnt_path).read()) + 1 if os.path.exists(cnt_path) else 1
    open(cnt_path, "w").write(str(cnt))
    data = bytearray(flash[off:off + size])
    if mode == "bad_second_read" and cnt == 2:
        data[0] ^= 0x01
    if mode == "corrupt_after_write" and cnt == 3:
        data[0] ^= 0x80
    open(out, "wb").write(data)
    sys.exit(0)

if "-f" in args or "--write-flash" in args:
    if "--verify" not in args:
        sys.stderr.write("missing --verify\\n")
        sys.exit(2)
    off = int(args[args.index("-o") + 1], 0)
    inp = args[-1]
    data = open(inp, "rb").read()
    flash[off:off + len(data)] = data
    open(flash_path, "wb").write(flash)
    sys.exit(0)

sys.stderr.write("unexpected args: %r\\n" % (args,))
sys.exit(2)
"""


def make_stub(tmp, flash_size, kv_addr, kv_bytes, mode="ok"):
    stub = os.path.join(tmp, "fake_ofl.py")
    with open(stub, "w") as f:
        f.write(STUB_LOADER)
    os.chmod(stub, os.stat(stub).st_mode | stat.S_IXUSR)

    flash = bytearray([0xFF]) * flash_size
    flash[kv_addr:kv_addr + len(kv_bytes)] = kv_bytes
    with open(os.path.join(tmp, "flash.bin"), "wb") as f:
        f.write(flash)
    with open(os.path.join(tmp, "mode.txt"), "w") as f:
        f.write(mode)
    return stub


def run_flash_sw(args, env_extra=None):
    env = dict(os.environ)
    if env_extra:
        env.update(env_extra)
    return subprocess.run(
        [sys.executable, FLASH_SW] + args,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=env,
    )


def test_preserve_populated_store(lay):
    kv_off = lay["offsets"]["kv"]
    kv_size = lay["offsets"]["kv_size"]
    region = lay["region_size"]
    base = 0x100000
    kv_data = (b"ZKV1" + bytes(range(256)) * 32)[:kv_size]
    sw_data = b"\x13\x00\x00\x00" * (0xF9380 // 4)

    with tempfile.TemporaryDirectory() as tmp:
        stub = make_stub(tmp, 0x200000, base + kv_off, kv_data)
        sw_path = os.path.join(tmp, "zeitlos_sw.bin")
        open(sw_path, "wb").write(sw_data)
        r = run_flash_sw(
            ["--base", "0x100000", "--flash-cmd", "%s -v -b ulx3s -f" % stub, sw_path],
            {"FAKE_OFL_DIR": tmp},
        )
        assert r.returncode == 0, r.stdout + r.stderr
        flash = open(os.path.join(tmp, "flash.bin"), "rb").read()
        assert flash[base:base + len(sw_data)] == sw_data
        assert flash[base + len(sw_data):base + kv_off] == b"\xff" * (kv_off - len(sw_data))
        assert flash[base + kv_off:base + region] == kv_data
        assert hashlib.md5(kv_data).hexdigest() in r.stdout
        assert "ZKV1" not in r.stdout


def test_fresh_empty_store(lay):
    kv_off = lay["offsets"]["kv"]
    kv_size = lay["offsets"]["kv_size"]
    base = 0x100000
    kv_data = b"\xff" * kv_size
    sw_data = b"\x55" * 0x30000

    with tempfile.TemporaryDirectory() as tmp:
        stub = make_stub(tmp, 0x200000, base + kv_off, kv_data)
        sw_path = os.path.join(tmp, "zeitlos_sw.bin")
        open(sw_path, "wb").write(sw_data)
        r = run_flash_sw(
            ["--base", "0x100000", "--flash-cmd", "%s -v -c ft2232 -f" % stub, sw_path],
            {"FAKE_OFL_DIR": tmp},
        )
        assert r.returncode == 0, r.stdout + r.stderr
        assert "empty, all 0xFF" in r.stdout


def test_mismatched_reads_refuse_write(lay):
    kv_off = lay["offsets"]["kv"]
    kv_size = lay["offsets"]["kv_size"]
    base = 0x100000
    kv_data = b"\xAA" * kv_size
    sw_data = b"\x11" * 0x1000

    with tempfile.TemporaryDirectory() as tmp:
        stub = make_stub(tmp, 0x200000, base + kv_off, kv_data, mode="bad_second_read")
        sw_path = os.path.join(tmp, "zeitlos_sw.bin")
        open(sw_path, "wb").write(sw_data)
        r = run_flash_sw(
            ["--base", "0x100000", "--flash-cmd", "%s -v -b ulx3s -f" % stub, sw_path],
            {"FAKE_OFL_DIR": tmp},
        )
        assert r.returncode != 0
        assert "differ" in r.stderr
        flash = open(os.path.join(tmp, "flash.bin"), "rb").read()
        # Nothing was written at base.
        assert flash[base:base + len(sw_data)] == b"\xff" * len(sw_data)
        calls = open(os.path.join(tmp, "calls.jsonl")).read().splitlines()
        assert len(calls) == 2


def test_post_write_mismatch_fails(lay):
    kv_off = lay["offsets"]["kv"]
    kv_size = lay["offsets"]["kv_size"]
    base = 0x100000
    kv_data = b"\x5A" * kv_size
    sw_data = b"\x22" * 0x1000

    with tempfile.TemporaryDirectory() as tmp:
        stub = make_stub(tmp, 0x200000, base + kv_off, kv_data, mode="corrupt_after_write")
        sw_path = os.path.join(tmp, "zeitlos_sw.bin")
        open(sw_path, "wb").write(sw_data)
        r = run_flash_sw(
            ["--base", "0x100000", "--flash-cmd", "%s -v -b ulx3s -f" % stub, sw_path],
            {"FAKE_OFL_DIR": tmp},
        )
        assert r.returncode != 0
        assert "changed after write" in r.stderr


def test_oversized_image_refused(lay):
    kv_off = lay["offsets"]["kv"]
    with tempfile.TemporaryDirectory() as tmp:
        sw_path = os.path.join(tmp, "big.bin")
        open(sw_path, "wb").write(b"\x00" * (kv_off + 1))
        r = run_flash_sw(["--base", "0x100000", "--dry-run", "--kv-empty", sw_path])
        assert r.returncode != 0
        assert "reaches the key/value store" in r.stderr


def test_artix7_base_and_flags(lay):
    kv_off = lay["offsets"]["kv"]
    kv_size = lay["offsets"]["kv_size"]
    region = lay["region_size"]
    base = 0x300000
    kv_data = b"\x33" * kv_size
    sw_data = b"\x44" * 0x2000

    with tempfile.TemporaryDirectory() as tmp:
        stub = make_stub(tmp, 0x400000, base + kv_off, kv_data)
        sw_path = os.path.join(tmp, "zeitlos_sw.bin")
        open(sw_path, "wb").write(sw_data)
        r = run_flash_sw(
            [
                "--base", "0x300000",
                "--flash-cmd", "%s -c dirtyJtag -f --fpga-part xc7a35tftg256" % stub,
                sw_path,
            ],
            {"FAKE_OFL_DIR": tmp},
        )
        assert r.returncode == 0, r.stdout + r.stderr
        flash = open(os.path.join(tmp, "flash.bin"), "rb").read()
        assert flash[base:base + len(sw_data)] == sw_data
        assert flash[base + kv_off:base + region] == kv_data
        calls = open(os.path.join(tmp, "calls.jsonl")).read()
        assert "--fpga-part" in calls
        assert str(base + kv_off) in calls


def main():
    lay = layout.load(ROOT)
    tests = [
        test_preserve_populated_store,
        test_fresh_empty_store,
        test_mismatched_reads_refuse_write,
        test_post_write_mismatch_fails,
        test_oversized_image_refused,
        test_artix7_base_and_flags,
    ]
    for fn in tests:
        fn(lay)
    print("flash_sw: %d tests passed" % len(tests))


if __name__ == "__main__":
    main()
