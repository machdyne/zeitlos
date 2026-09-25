#!/usr/bin/env python3
#
# Zeitlos
# Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
#
# Captures a Meshtastic node's client-API byte stream on a Linux host,
# for sw/apps/mesh's test vectors. See docs/mesh_app.md, "Phase 0".
#
#   python3 tools/mesh_capture.py /dev/ttyUSB0 [seconds] [out.bin]
#
# Opens the port at 115200, wakes the node's API, asks for its whole
# configuration (ToRadio.want_config_id with a random nonce), and then
# records EVERYTHING that comes back -- frames and console text alike,
# byte for byte -- for `seconds` (default 30). Send a message from a
# phone during the capture to get live traffic in it too.
#
# Needs pyserial (BSD) and nothing else. No Meshtastic library, no
# protobuf library and no .proto files: the two messages it sends are
# written out by hand below, the same way sw/apps/mesh builds them.
#
# The output is raw bytes. It is data from your own node -- including
# its node DB, which holds names and positions of nodes it has heard --
# so look at it before committing it anywhere public; the summary this
# prints says what is in it.

import os
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("needs pyserial: pip install pyserial")

START1, START2 = 0x94, 0xC3


def varint(v):
    out = bytearray()
    while v >= 0x80:
        out.append((v & 0x7F) | 0x80)
        v >>= 7
    out.append(v)
    return bytes(out)


def frame(pb):
    return bytes([START1, START2, len(pb) >> 8, len(pb) & 0xFF]) + pb


def want_config(nonce):
    # ToRadio field 3 (want_config_id), varint
    return frame(bytes([3 << 3 | 0]) + varint(nonce))


def summarize(data):
    """Walk the capture the way mesh_frame.c does and count what is in it."""
    frames, text, resync = [], bytearray(), 0
    i = 0
    while i < len(data):
        if data[i] == START1 and i + 3 < len(data) and data[i + 1] == START2:
            n = data[i + 2] << 8 | data[i + 3]
            if n <= 512 and i + 4 + n <= len(data):
                frames.append(data[i + 4:i + 4 + n])
                i += 4 + n
                continue
            resync += 1
        text.append(data[i])
        i += 1
    kinds = {}
    for f in frames:
        # FromRadio's payload field is the first non-id field; its
        # number says which kind of message this is.
        j, kind = 0, None
        while j < len(f):
            tag, j = read_varint(f, j)
            if tag is None:
                break
            num, wt = tag >> 3, tag & 7
            if num != 1:
                kind = num
                break
            j = skip(f, j, wt)
            if j is None:
                break
        kinds[kind] = kinds.get(kind, 0) + 1
    return frames, text, resync, kinds


def read_varint(b, j):
    v, s = 0, 0
    while j < len(b) and s < 70:
        c = b[j]
        j += 1
        v |= (c & 0x7F) << s
        s += 7
        if not c & 0x80:
            return v, j
    return None, j


def skip(b, j, wt):
    if wt == 0:
        return read_varint(b, j)[1]
    if wt == 1:
        return j + 8
    if wt == 5:
        return j + 4
    if wt == 2:
        n, j = read_varint(b, j)
        return None if n is None else j + n
    return None


# FromRadio field numbers, for the summary only.
NAMES = {2: "packet", 3: "my_info", 4: "node_info", 5: "config",
         6: "log_record", 7: "config_complete_id", 8: "rebooted",
         9: "moduleConfig", 10: "channel", 11: "queueStatus",
         13: "metadata", 15: "fileInfo", 16: "clientNotification",
         17: "deviceuiConfig"}


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: mesh_capture.py PORT [seconds] [out.bin]")
    port = sys.argv[1]
    secs = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0
    out = sys.argv[3] if len(sys.argv) > 3 else "mesh_capture.bin"

    nonce = int.from_bytes(os.urandom(4), "little") & 0x7FFFFFFF
    s = serial.Serial(port, 115200, timeout=0.1)
    time.sleep(0.2)
    s.reset_input_buffer()
    s.write(bytes([START2]) * 32)      # wake: resynchronise its receiver
    time.sleep(0.1)
    s.write(want_config(nonce))

    data = bytearray()
    t0 = time.time()
    while time.time() - t0 < secs:
        data += s.read(4096)
    s.close()

    with open(out, "wb") as f:
        f.write(data)

    frames, text, resync, kinds = summarize(bytes(data))
    print(f"{len(data)} bytes in {secs:.0f} s -> {out}")
    print(f"nonce {nonce}; {len(frames)} frames, {len(text)} console bytes, "
          f"{resync} bad headers")
    for k in sorted(kinds, key=lambda x: (x is None, x)):
        print(f"  {NAMES.get(k, k)}: {kinds[k]}")
    done = any(fr[:1] and nonce_in(fr, nonce) for fr in frames)
    print("config_complete_id matched" if done else
          "NO config_complete_id with our nonce -- capture incomplete?")


def nonce_in(f, nonce):
    j = 0
    while j < len(f):
        tag, j = read_varint(f, j)
        if tag is None:
            return False
        if tag == (7 << 3 | 0):
            v, j = read_varint(f, j)
            return v == nonce
        j = skip(f, j, tag & 7)
        if j is None:
            return False
    return False


if __name__ == "__main__":
    main()
