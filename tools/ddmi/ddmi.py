#!/usr/bin/env python3
"""
DDMI reference model and stream checker -- tools/ddmi/ddmi.py

The bench for DDMI audio (docs/ddmi_audio.md). Two halves:

  ENCODE   what a correct source puts on the three TMDS data channels,
           character by character: video (8b/10b TMDS with DC balance,
           exactly as rtl/gpu/tmds_encoder.v), control tokens, TERC4
           data-island characters, guard bands, preambles; and the
           packets carried in data islands (header + four subpackets,
           BCH parity) with the InfoFrames and audio packets built the
           way the gateware will build them.

  DECODE   a checker that reads a capture of the three channels (one
           10-bit character per pixel clock per channel, as
           rtl/gpu/bench/tb_ddmi_capture.v writes it) and rebuilds
           everything from the symbols alone: sync, pixels, control
           periods, preambles, guard bands, every packet with its BCH
           parity checked, InfoFrame checksums, audio samples with
           their IEC 60958 parity and channel status, clock
           regeneration. It knows nothing about the RTL that made the
           stream, the same rule as rtl/tb/tb_audio_spdif.v's receiver.

Written from public material only -- CEA-861 for the InfoFrames,
IEC 60958 for channel status, and the open-source TMDS/TERC4/BCH
implementations it is cross-checked against in test_ddmi.py. No
HDMI-specific identifiers are used or emitted.

Bit order everywhere: a 10-bit character is an int with q_out[0] in
bit 0, the bit sent first.
"""

import os
import sys

# -- control tokens: (C1, C0) -> character, DVI 1.0 -----------------------
CTRL = {0: 0b1101010100, 1: 0b0010101011, 2: 0b0101010100, 3: 0b1010101011}
CTRL_INV = {v: k for k, v in CTRL.items()}

# -- TERC4: 4 bits -> character -------------------------------------------
TERC4 = [
    0b1010011100, 0b1001100011, 0b1011100100, 0b1011100010,
    0b0101110001, 0b0100011110, 0b0110001110, 0b0100111100,
    0b1011001100, 0b0100111001, 0b0110011100, 0b1011000110,
    0b1010001110, 0b1001110001, 0b0101100011, 0b1011000011,
]
TERC4_INV = {v: i for i, v in enumerate(TERC4)}

# -- guard bands -----------------------------------------------------------
VIDEO_GB = (0b1011001100, 0b0100110011, 0b1011001100)   # ch0, ch1, ch2
DATA_GB_12 = 0b0100110011                               # ch1 and ch2
def data_gb0(vsync, hsync):                             # ch0: TERC4
    return TERC4[0b1100 | (vsync << 1) | hsync]

# Preambles: CTL0..CTL3 = (ch1 C0, ch1 C1, ch2 C0, ch2 C1).
PRE_VIDEO = (1, 0, 0, 0)
PRE_DATA = (1, 0, 1, 0)


def popcount(x):
    return bin(x).count("1")


class TmdsEncoder:
    """One channel's video encoder, bit-identical to tmds_encoder.v
    (DVI 1.0 p.29). `cnt` is the running disparity; it is reset by any
    non-video character, as the RTL does."""

    def __init__(self):
        self.cnt = 0

    def video(self, d):
        n1d = popcount(d)
        qm = d & 1
        xnor = n1d > 4 or (n1d == 4 and (d & 1) == 0)
        for i in range(1, 8):
            prev = (qm >> (i - 1)) & 1
            b = (d >> i) & 1
            bit = (1 - (prev ^ b)) if xnor else (prev ^ b)
            qm |= bit << i
        q8 = 0 if xnor else 1
        n1 = popcount(qm & 0xFF)
        n0 = 8 - n1
        if self.cnt == 0 or n1 == n0:
            q9 = 1 - q8
            low = qm if q8 else (~qm & 0xFF)
            if q8 == 0:
                self.cnt += n0 - n1
            else:
                self.cnt += n1 - n0
        elif (self.cnt > 0 and n1 > n0) or (self.cnt < 0 and n0 > n1):
            q9 = 1
            low = ~qm & 0xFF
            self.cnt += 2 * q8 + (n0 - n1)
        else:
            q9 = 0
            low = qm
            self.cnt += -2 * (1 - q8) + (n1 - n0)
        return low | (q8 << 8) | (q9 << 9)

    def other(self, ch):
        self.cnt = 0
        return ch


def tmds_decode(q):
    """10-bit video character -> 8-bit value."""
    low = q & 0xFF
    if (q >> 9) & 1:
        low = ~low & 0xFF
    d = low & 1
    for i in range(1, 8):
        a = (low >> i) & 1
        b = (low >> (i - 1)) & 1
        bit = (a ^ b) if (q >> 8) & 1 else (1 - (a ^ b))
        d |= bit << i
    return d


# -- BCH parity for data-island packets -----------------------------------
#
# G(x) = 1 + x^6 + x^7 + x^8, as an LSB-first LFSR (mask 0x83). The
# header is BCH(32,24) over HB0..HB2 and each subpacket BCH(64,56) over
# SB0..SB6, bits taken in transmission order (byte 0 first, LSB first).
def bch_ecc(data_bytes):
    p = 0
    for byte in data_bytes:
        for i in range(8):
            fb = (p ^ (byte >> i)) & 1
            p >>= 1
            if fb:
                p ^= 0x83
    return p


class Packet:
    """HB0..HB2 and four subpackets of SB0..SB6. Parity is computed when
    the packet is serialised."""

    def __init__(self, hb, sb=None):
        self.hb = list(hb)
        self.sb = [list(s) for s in (sb or [[0] * 7] * 4)]

    def header_bits(self):
        bs = self.hb + [bch_ecc(self.hb)]
        return [(bs[i // 8] >> (i % 8)) & 1 for i in range(32)]

    def sub_bits(self, k):
        bs = self.sb[k] + [bch_ecc(self.sb[k])]
        return [(bs[i // 8] >> (i % 8)) & 1 for i in range(64)]

    def chars(self, vsync, hsync, first_in_island):
        """32 (ch0, ch1, ch2) TERC4 4-bit values."""
        hbits = self.header_bits()
        sbits = [self.sub_bits(k) for k in range(4)]
        out = []
        for c in range(32):
            b3 = 0 if (first_in_island and c == 0) else 1
            ch0 = hsync | (vsync << 1) | (hbits[c] << 2) | (b3 << 3)
            ch1 = sum(sbits[k][2 * c] << k for k in range(4))
            ch2 = sum(sbits[k][2 * c + 1] << k for k in range(4))
            out.append((ch0, ch1, ch2))
        return out

    def __eq__(self, o):
        return self.hb == o.hb and self.sb == o.sb

    def __repr__(self):
        return "Packet(%s)" % " ".join("%02x" % b for b in self.hb)


def null_packet():
    return Packet([0, 0, 0])


# -- packets --------------------------------------------------------------

def infoframe(type_, version, payload):
    """CEA-861 InfoFrame: HB0 = type, HB1 = version, HB2 = length, PB0 =
    checksum so that every byte of header and payload sums to 0, then
    PB1..PBn. PB[7k + j] is subpacket k byte j."""
    n = len(payload)
    hb = [type_, version, n]
    csum = (-(sum(hb) + sum(payload))) & 0xFF
    pb = [csum] + list(payload) + [0] * (27 - n)
    sb = [pb[7 * k:7 * k + 7] for k in range(4)]
    return Packet(hb, sb)


def avi_infoframe(vic=1, aspect_43=True, full_range=True, it=True, underscan=True):
    """AVI InfoFrame, version 2, 13 bytes. Defaults are Zeitlos's: RGB,
    active format = picture, 4:3, underscan, IT content (graphics),
    full-range RGB, VIC 1 (640x480p)."""
    pb1 = (0 << 5) | (1 << 4) | (0 << 2) | (2 if underscan else 0)
    pb2 = (0 << 6) | ((1 if aspect_43 else 2) << 4) | 0x8
    pb3 = ((1 if it else 0) << 7) | ((2 if full_range else 0) << 2)
    pb4 = vic
    pb5 = 0          # content type graphics (with ITC), no repetition
    return infoframe(0x82, 2, [pb1, pb2, pb3, pb4, pb5] + [0] * 8)


def audio_infoframe(channels=2):
    """Audio InfoFrame, version 1, 10 bytes: coding type, sample size and
    rate 'refer to stream header', channel count, FL/FR."""
    return infoframe(0x84, 1, [channels - 1, 0, 0, 0x00, 0] + [0] * 5)


def spd_infoframe(vendor="Zeitlos", product="Zeitlos", source=0x09):
    """Source Product Description InfoFrame, version 1, 25 bytes. 0x09
    is 'PC general'."""
    v = vendor.encode("ascii")[:8].ljust(8, b"\0")
    p = product.encode("ascii")[:16].ljust(16, b"\0")
    return infoframe(0x83, 1, list(v) + list(p) + [source])


def gcp(set_avmute=False, clear_avmute=False):
    sb0 = (1 if set_avmute else 0) | ((1 if clear_avmute else 0) << 4)
    return Packet([0x03, 0, 0], [[sb0, 0, 0, 0, 0, 0, 0]] * 4)


def acr(n, cts):
    """Audio clock regeneration: the sink rebuilds 128 fs as
    f_TMDS * N / CTS."""
    sb = [0, (cts >> 16) & 0xF, (cts >> 8) & 0xFF, cts & 0xFF,
          (n >> 16) & 0xF, (n >> 8) & 0xFF, n & 0xFF]
    return Packet([0x01, 0, 0], [sb] * 4)


def even_parity(x):
    return popcount(x) & 1


def audio_sample_packet(frames, first_index, cs_bits):
    """Up to four stereo frames of signed 16-bit samples, two channels.
    `first_index` is the frame's position in the 192-frame IEC 60958
    block (frame 0 carries B); cs_bits(index) -> channel-status bit.
    A 16-bit sample sits in the TOP 16 of the 24-bit field."""
    assert 1 <= len(frames) <= 4
    present = (1 << len(frames)) - 1
    bflags = 0
    sb = []
    for k, (l, r) in enumerate(frames):
        idx = (first_index + k) % 192
        if idx == 0:
            bflags |= 1 << k
        c = cs_bits(idx)
        sub = []
        for s in (l & 0xFFFF, r & 0xFFFF):
            sub += [0, s & 0xFF, s >> 8]
        # V = 0 (valid), U = 0, C = channel status, P = even parity
        # over the 24 sample bits and V, U, C.
        lpar = even_parity((l & 0xFFFF) << 8) ^ c
        rpar = even_parity((r & 0xFFFF) << 8) ^ c
        sub.append((c << 2) | (lpar << 3) | (c << 6) | (rpar << 7))
        sb.append(sub)
    while len(sb) < 4:
        sb.append([0] * 7)
    return Packet([0x02, present, bflags << 4], sb)


def channel_status_bits(fs_code):
    """IEC 60958 consumer channel status, 192 bits, as audio_spdif.v
    sends it: consumer, PCM, copy permitted (bit 2), category 0,
    sampling frequency in bits 24-27 (fs_code[0] = bit 24; 2 = 48 kHz,
    0 = 44.1 kHz, 3 = 32 kHz)."""
    bits = [0] * 192
    bits[2] = 1
    for i in range(4):
        bits[24 + i] = (fs_code >> i) & 1
    return bits


# -- the line/frame layout DDMI audio uses ---------------------------------
#
# 640x480p60: 800 x 525, active x 160..799, y 45..524 in gpu_video.v's
# own counters (front porch first). One island per line at hc 12..87
# (preamble 12..19, guard 20..21, two packets 22..85, guard 86..87),
# video preamble 150..157, video guard 158..159. See docs/ddmi_audio.md.
H_TOTAL, V_TOTAL = 800, 525
H_ACTIVE0, V_ACTIVE0 = 160, 45
H_FP, H_SYNC = 16, 96
V_FP, V_SYNC = 10, 2
ISLAND_PRE, ISLAND_GB0, ISLAND_PKT, ISLAND_GB1 = 12, 20, 22, 86
VIDEO_PRE, VIDEO_GB0 = 150, 158


def syncs(hc, vc):
    """gpu_video.v's sync outputs as the encoder sees them: ACTIVE LOW
    (high except during the pulse) -- 640x480 has negative syncs. On
    channel 0, C0 = hsync and C1 = vsync."""
    hs = 0 if H_FP <= hc < H_FP + H_SYNC else 1
    vs = 0 if V_FP <= vc < V_FP + V_SYNC else 1
    return hs, vs


def _ctl(enc, hs, vs, ctl=(0, 0, 0, 0)):
    return (enc[0].other(CTRL[(vs << 1) | hs]),
            enc[1].other(CTRL[(ctl[1] << 1) | ctl[0]]),
            enc[2].other(CTRL[(ctl[3] << 1) | ctl[2]]))


def encode_frames(pixel, packets_for_line=None, frames=1):
    """Characters for whole frames: a list of (ch0, ch1, ch2) per pixel
    clock, starting at hc = vc = 0.

    pixel(x, y) -> (r, g, b) 8-bit. packets_for_line(frame, vc) -> None
    for plain DVI (no islands, no video preamble or guard band), or a
    list of up to two Packets for that line's island ([] for an
    HDMI-mode line with no island -- preamble and guard band still
    precede video). Channel 0 is blue and carries the syncs, 1 is
    green, 2 is red, as in gpu_ddmi.v.
    """
    enc = [TmdsEncoder() for _ in range(3)]
    out = []
    for f in range(frames):
        for vc in range(V_TOTAL):
            pk = packets_for_line(f, vc) if packets_for_line else None
            hdmi = pk is not None
            if pk:
                pk = list(pk) + [null_packet()] * (2 - len(pk))
            for hc in range(H_TOTAL):
                hs, vs = syncs(hc, vc)
                if vc >= V_ACTIVE0 and hc >= H_ACTIVE0:
                    rgb = pixel(hc - H_ACTIVE0, vc - V_ACTIVE0)
                    out.append(tuple(enc[c].video(rgb[2 - c]) for c in range(3)))
                elif pk and ISLAND_PRE <= hc < ISLAND_GB0:
                    out.append(_ctl(enc, hs, vs, PRE_DATA))
                elif pk and (ISLAND_GB0 <= hc < ISLAND_PKT or ISLAND_GB1 <= hc < ISLAND_GB1 + 2):
                    out.append((enc[0].other(data_gb0(vs, hs)),
                                enc[1].other(DATA_GB_12), enc[2].other(DATA_GB_12)))
                elif pk and ISLAND_PKT <= hc < ISLAND_GB1:
                    i = hc - ISLAND_PKT
                    c0, c1, c2 = pk[i // 32].chars(vs, hs, i < 32)[i % 32]
                    out.append((enc[0].other(TERC4[c0]),
                                enc[1].other(TERC4[c1]), enc[2].other(TERC4[c2])))
                elif hdmi and vc >= V_ACTIVE0 and VIDEO_PRE <= hc < VIDEO_GB0:
                    out.append(_ctl(enc, hs, vs, PRE_VIDEO))
                elif hdmi and vc >= V_ACTIVE0 and VIDEO_GB0 <= hc < H_ACTIVE0:
                    out.append(tuple(enc[c].other(VIDEO_GB[c]) for c in range(3)))
                else:
                    out.append(_ctl(enc, hs, vs))
    return out


# =========================================================================
# DECODE
# =========================================================================

class Report:
    def __init__(self):
        self.errors = []
        self.lines = []          # per line: dict(hs_at, mode, video_len, ...)
        self.packets = []        # (frame, line, char_index, Packet)
        self.frames = []         # list of frames, each a list of pixel rows
        self.mode = None         # 'dvi' or 'hdmi'
        self.ext_ctl_frames = 0

    def err(self, where, msg):
        if len(self.errors) < 200:
            self.errors.append("%s: %s" % (where, msg))

    def ok(self):
        return not self.errors


def read_capture(path):
    """One line per pixel clock: three hex 10-bit characters, ch0 ch1
    ch2. Lines starting with # are comments."""
    out = []
    with open(path) as f:
        for ln in f:
            if not ln.strip() or ln[0] == "#":
                continue
            a, b, c = ln.split()[:3]
            out.append((int(a, 16), int(b, 16), int(c, 16)))
    return out


def unpack_packet(chars):
    """32 (ch0, ch1, ch2) TERC4 values -> (Packet, header_ok, sub_ok[4],
    bit3 list)."""
    hbits = [(c[0] >> 2) & 1 for c in chars]
    sbits = [[0] * 64 for _ in range(4)]
    for i, (_, c1, c2) in enumerate(chars):
        for k in range(4):
            sbits[k][2 * i] = (c1 >> k) & 1
            sbits[k][2 * i + 1] = (c2 >> k) & 1

    def tobytes(bits):
        return [sum(bits[8 * j + i] << i for i in range(8)) for j in range(len(bits) // 8)]

    hb = tobytes(hbits)
    sb = [tobytes(b) for b in sbits]
    p = Packet(hb[:3], [s[:7] for s in sb])
    hok = bch_ecc(hb[:3]) == hb[3]
    sok = [bch_ecc(s[:7]) == s[7] for s in sb]
    return p, hok, sok


def decode(chars, frames_expected=None):
    """Walk a capture as a receiver would. Returns a Report."""
    rep = Report()
    n = len(chars)
    # find line starts: hsync falling edge on ch0 (control tokens or
    # TERC4/guard all carry hsync in bit 0)
    def hs_of(c0):
        if c0 in CTRL_INV:
            return CTRL_INV[c0] & 1, (CTRL_INV[c0] >> 1) & 1
        if c0 in TERC4_INV:
            v = TERC4_INV[c0]
            return v & 1, (v >> 1) & 1
        return None, None

    i = 0
    state = "ctl"
    ctl_run = 0            # control characters in a row (incl. preamble)
    pre_kind = None        # 'video' or 'data' once 8 preamble chars seen
    pre_run = 0
    pre_max = 0            # longest preamble run in this control period
    hdmi_seen = False
    frame = -1
    line = -1
    prev_vs = None
    prev_hs = None
    row = None
    rows = None
    ext_this_frame = False

    while i < n:
        c0, c1, c2 = chars[i]
        # Syncs are read from channel 0 only where it carries them: a
        # control or data-island character. A video character can
        # happen to equal a TERC4 code, so it must not be asked.
        hs = vs = None
        if state == "ctl" and c0 in CTRL_INV:
            hs, vs = CTRL_INV[c0] & 1, CTRL_INV[c0] >> 1
        elif state == "island" and c0 in TERC4_INV:
            hs, vs = TERC4_INV[c0] & 1, (TERC4_INV[c0] >> 1) & 1

        # line and frame bookkeeping from the sync edges (active low)
        if hs is not None:
            if prev_hs == 1 and hs == 0:
                line += 1
            if prev_vs == 1 and vs == 0:
                if rows is not None:
                    rep.frames.append(rows)
                    if hdmi_seen and not ext_this_frame:
                        rep.err("frame %d" % frame, "no extended control period (>= 32 control characters)")
                    if ext_this_frame:
                        rep.ext_ctl_frames += 1
                frame += 1
                rows = []
                line = 0
                ext_this_frame = False
            prev_hs, prev_vs = hs, vs
        where = "f%d l%d c%d" % (frame, line, i)

        if state == "ctl":
            if c0 in CTRL_INV and c1 in CTRL_INV and c2 in CTRL_INV:
                ctl = (CTRL_INV[c1] & 1, CTRL_INV[c1] >> 1, CTRL_INV[c2] & 1, CTRL_INV[c2] >> 1)
                ctl_run += 1
                if ctl == (0, 0, 0, 0):
                    pre_run = 0
                    pre_kind = None
                    if ctl_run >= 32:
                        ext_this_frame = True
                elif ctl in (PRE_VIDEO, PRE_DATA):
                    k = "video" if ctl == PRE_VIDEO else "data"
                    if pre_kind != k:
                        pre_kind, pre_run = k, 0
                    pre_run += 1
                    pre_max = max(pre_max, pre_run)
                else:
                    rep.err(where, "control period with CTL %s" % (ctl,))
                i += 1
                continue
            # not a control character: what follows a control period
            if pre_kind and pre_run >= 8:
                if pre_run != 8:
                    rep.err(where, "%s preamble of %d characters, want 8" % (pre_kind, pre_run))
                if ctl_run < 12:
                    rep.err(where, "control period of %d characters before a %s period, want >= 12" % (ctl_run, pre_kind))
                hdmi_seen = True
                if pre_kind == "video":
                    if (c0, c1, c2) != VIDEO_GB or chars[i + 1] != VIDEO_GB:
                        rep.err(where, "video guard band missing")
                    i += 2
                    state = "video"
                else:
                    if c1 != DATA_GB_12 or c2 != DATA_GB_12 or chars[i + 1][1:] != (DATA_GB_12, DATA_GB_12):
                        rep.err(where, "data island leading guard band missing")
                    for j in (i, i + 1):
                        v = TERC4_INV.get(chars[j][0])
                        if v is None or (v >> 2) != 0b11:
                            rep.err(where, "data island guard band ch0 is not TERC4(11vh)")
                    i += 2
                    state = "island"
                    island_pkts = 0
                    island_start = i
                ctl_run = 0
                pre_kind, pre_run, pre_max = None, 0, 0
                continue
            if hdmi_seen:
                rep.err(where, "data or video without a preamble")
            elif pre_max:
                rep.err(where, "preamble of %d characters, want 8" % pre_max)
            # DVI: straight into video
            state = "video"
            ctl_run = 0
            pre_max = 0
            continue

        if state == "video":
            if c0 in CTRL_INV:
                if row is not None:
                    if rows is not None:
                        rows.append(row)
                    row = None
                state = "ctl"
                continue
            if row is None:
                row = []
            row.append((tmds_decode(c2), tmds_decode(c1), tmds_decode(c0)))
            i += 1
            continue

        if state == "island":
            if c1 == DATA_GB_12:
                # trailing guard band
                if chars[i + 1][1] != DATA_GB_12:
                    rep.err(where, "trailing guard band is 1 character")
                for j in (i, i + 1):
                    v = TERC4_INV.get(chars[j][0])
                    if v is None or (v >> 2) != 0b11:
                        rep.err(where, "trailing guard band ch0 is not TERC4(11vh)")
                if island_pkts == 0:
                    rep.err(where, "data island with no packets")
                if island_pkts > 18:
                    rep.err(where, "data island of %d packets, max 18" % island_pkts)
                i += 2
                state = "ctl"
                ctl_run = 0
                continue
            pk = chars[i:i + 32]
            if len(pk) < 32:
                break
            vals = []
            for j, (a, b, c) in enumerate(pk):
                if a not in TERC4_INV or b not in TERC4_INV or c not in TERC4_INV:
                    rep.err(where, "non-TERC4 character inside a packet at +%d" % j)
                    vals.append((0, 0, 0))
                    continue
                vals.append((TERC4_INV[a], TERC4_INV[b], TERC4_INV[c]))
            for j, v in enumerate(vals):
                want3 = 0 if (island_pkts == 0 and j == 0) else 1
                if ((v[0] >> 3) & 1) != want3:
                    rep.err(where, "ch0 bit 3 is %d at packet %d char %d, want %d" % ((v[0] >> 3) & 1, island_pkts, j, want3))
                    break
            p, hok, sok = unpack_packet(vals)
            if not hok:
                rep.err(where, "header BCH parity wrong %r" % p)
            for k in range(4):
                if not sok[k]:
                    rep.err(where, "subpacket %d BCH parity wrong %r" % (k, p))
            rep.packets.append((frame, line, i, p))
            island_pkts += 1
            i += 32
            continue

    if rows:
        rep.frames.append(rows)
    rep.mode = "hdmi" if hdmi_seen else "dvi"
    return rep


# -- interpreting packets ---------------------------------------------------

PKT_NAMES = {0x00: "null", 0x01: "ACR", 0x02: "audio", 0x03: "GCP",
             0x82: "AVI", 0x83: "SPD", 0x84: "audio-IF"}


def check_packets(rep):
    """Content checks on every decoded packet. Returns a summary dict:
    counts by type, InfoFrames seen, ACR (N, CTS) values, audio frames
    as (l, r) in order, and the channel status blocks recovered."""
    summ = {"count": {}, "acr": set(), "infoframes": {}, "audio": [],
            "cs_blocks": [], "gcp": []}
    cs_cur = None
    for (f, l, i, p) in rep.packets:
        t = p.hb[0]
        name = PKT_NAMES.get(t, "0x%02x" % t)
        summ["count"][name] = summ["count"].get(name, 0) + 1
        where = "f%d l%d c%d %s" % (f, l, i, name)
        if t == 0x00:
            if p.hb != [0, 0, 0] or any(any(s) for s in p.sb):
                rep.err(where, "null packet is not all zero")
        elif t == 0x01:
            vals = set()
            for s in p.sb:
                cts = (s[1] << 16) | (s[2] << 8) | s[3]
                nn = (s[4] << 16) | (s[5] << 8) | s[6]
                vals.add((nn, cts))
            if len(vals) != 1:
                rep.err(where, "ACR subpackets differ")
            summ["acr"] |= vals
        elif t == 0x03:
            if any(s != p.sb[0] for s in p.sb):
                rep.err(where, "GCP subpackets differ")
            summ["gcp"].append(p.sb[0][0])
        elif t >= 0x80:
            ln = p.hb[2]
            pb = sum(p.sb, [])[:ln + 1]
            if (sum(p.hb) + sum(pb)) & 0xFF:
                rep.err(where, "InfoFrame checksum wrong")
            summ["infoframes"][name] = (p.hb[1], pb[1:])
        elif t == 0x02:
            present = p.hb[1] & 0xF
            if (p.hb[1] >> 4) & 1:
                rep.err(where, "layout 1 (more than two channels)")
            bflags = p.hb[2] >> 4
            for k in range(4):
                if not (present >> k) & 1:
                    if any(p.sb[k]):
                        rep.err(where, "subpacket %d not present but not zero" % k)
                    continue
                s = p.sb[k]
                lft = s[1] | (s[2] << 8)
                rgt = s[4] | (s[5] << 8)
                if s[0] or s[3]:
                    rep.err(where, "low byte of a 16-bit sample is not zero")
                fl = s[6]
                vl, ul, cl, pl = fl & 1, (fl >> 1) & 1, (fl >> 2) & 1, (fl >> 3) & 1
                vr, ur, cr, pr = (fl >> 4) & 1, (fl >> 5) & 1, (fl >> 6) & 1, (fl >> 7) & 1
                if vl or vr:
                    rep.err(where, "sample marked invalid (V set)")
                if pl != (even_parity(lft << 8) ^ vl ^ ul ^ cl):
                    rep.err(where, "left parity wrong")
                if pr != (even_parity(rgt << 8) ^ vr ^ ur ^ cr):
                    rep.err(where, "right parity wrong")
                if cl != cr:
                    rep.err(where, "channel status differs between channels")
                if (bflags >> k) & 1:
                    if cs_cur is not None:
                        if len(cs_cur) != 192:
                            rep.err(where, "channel-status block of %d frames, want 192" % len(cs_cur))
                        summ["cs_blocks"].append(cs_cur)
                    cs_cur = []
                if cs_cur is not None:
                    cs_cur.append(cl)
                summ["audio"].append((lft - 65536 if lft & 0x8000 else lft,
                                      rgt - 65536 if rgt & 0x8000 else rgt))
    return summ


def cs_fields(bits):
    """Consumer channel status, the fields Zeitlos sets."""
    return {"consumer": bits[0] == 0, "pcm": bits[1] == 0,
            "copy_ok": bits[2] == 1,
            "fs_code": sum(bits[24 + i] << i for i in range(4))}


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(description="check a DDMI capture")
    ap.add_argument("capture")
    ap.add_argument("--expect", choices=["dvi", "hdmi"], help="required mode")
    ap.add_argument("--pattern", help="tb_ddmi_capture's pixel pattern to check pixels against")
    ap.add_argument("--model", action="store_true",
                    help="also re-encode the picture with this model and "
                         "compare character for character")
    ap.add_argument("--schedule", choices=sorted(SCHEDULES), default="dvi",
                    help="what --model puts between the pixels (default dvi)")
    ap.add_argument("--sha256", help="file holding the expected sha256 of the capture's characters")
    ap.add_argument("--write-sha256", help="write the capture's sha256 to this file (making a golden)")
    ap.add_argument("--frame-islands", help="expected islands in each whole "
                    "frame of the capture (it starts at a frame boundary), e.g. 0,525")
    ap.add_argument("--audio-seq", action="store_true",
                    help="the samples must be a run of tb_ddmi_capture's sequence "
                         "L(n) = 1237 n, R(n) = 4813 n + 77")
    ap.add_argument("--audio-rep", type=int, default=1,
                    help="each frame of the sequence is sent this many times")
    ap.add_argument("--audio-hz", type=float,
                    help="the samples must arrive at this rate (within 0.5%%), and "
                         "N/CTS must say so")
    ap.add_argument("-v", action="store_true")
    a = ap.parse_args(argv)
    chars = read_capture(a.capture)
    rep = decode(chars)
    summ = check_packets(rep)
    print("%s: %d characters, %d frames, mode %s" % (a.capture, len(chars), len(rep.frames), rep.mode))
    for fi, fr in enumerate(rep.frames):
        widths = set(len(r) for r in fr)
        print("  frame %d: %d active lines, widths %s" % (fi, len(fr), sorted(widths)))
        if len(fr) not in (0, 480) or (fr and widths != {640}):
            if fi > 0:
                rep.err("frame %d" % fi, "not 640x480")
    if rep.packets:
        print("  packets:", ", ".join("%s %d" % kv for kv in sorted(summ["count"].items())))
        if summ["acr"]:
            print("  ACR N/CTS:", sorted(summ["acr"]))
        for k, v in summ["infoframes"].items():
            print("  %s v%d: %s" % (k, v[0], " ".join("%02x" % b for b in v[1])))
        if summ["audio"]:
            print("  audio: %d frames" % len(summ["audio"]))
        for b in summ["cs_blocks"][:1]:
            print("  channel status:", cs_fields(b))
    if a.expect and rep.mode != a.expect:
        rep.err("mode", "is %s, expected %s" % (rep.mode, a.expect))
    if a.pattern:
        pat = PATTERNS[a.pattern]
        for fi, fr in enumerate(rep.frames):
            if len(fr) != 480:
                continue
            bad = 0
            for y, r in enumerate(fr):
                for x, px in enumerate(r):
                    if px != pat(x, y, fi):
                        bad += 1
                        if bad <= 3:
                            rep.err("frame %d (%d,%d)" % (fi, x, y), "pixel %r want %r" % (px, pat(x, y, fi)))
            print("  frame %d pixels: %s" % (fi, "ok" if not bad else "%d wrong" % bad))
    if a.model:
        full = [fr for fr in rep.frames if len(fr) == 480]
        if a.pattern:
            pat = PATTERNS[a.pattern]
            pixel = lambda x, y: pat(x, y, 0)
        elif full:
            # the picture as decoded: the model then checks everything
            # but the pixels, which --pattern or --sha256 cover
            fr0 = full[0]
            pixel = lambda x, y: fr0[y][x]
        else:
            pixel = None
            rep.err("model", "--model needs --pattern or a whole decoded frame")
        if pixel:
            ref = encode_frames(pixel, SCHEDULES[a.schedule], frames=1)
            # align on the first vsync falling edge: model index is
            # vc 10, hc 0; in the capture, the first control token on
            # ch0 with vsync low after one with it high
            def vfall(ch):
                prev = None
                for i, c in enumerate(ch):
                    if c[0] in CTRL_INV:
                        v = CTRL_INV[c[0]] >> 1
                        if prev == 1 and v == 0:
                            return i
                        prev = v
                return None
            ci, mi = vfall(chars), vfall(ref)
            n = min(len(chars) - ci, len(ref) - mi) if ci is not None and mi is not None else 0
            bad = [k for k in range(n) if chars[ci + k] != ref[mi + k]]
            print("  model: %d characters compared, %d differ" % (n, len(bad)))
            if n < 400000 or bad:
                rep.err("model", "%d of %d characters differ from the model%s" % (
                    len(bad), n, (", first at +%d: %r vs %r" % (bad[0], chars[ci + bad[0]], ref[mi + bad[0]])) if bad else ""))
    if a.frame_islands:
        want = [int(x) for x in a.frame_islands.split(",")]
        got = [0] * len(want)
        for (_, _, i, _) in rep.packets:
            k = i // (H_TOTAL * V_TOTAL)
            if k < len(got):
                got[k] += 1
        got = [g // 2 for g in got]      # two packets per island
        print("  islands per frame: %s (want %s)" % (got, want))
        if got != want:
            rep.err("frames", "islands per frame %s, want %s" % (got, want))
    if a.audio_seq or a.audio_hz:
        aud = summ["audio"]
        if not aud:
            rep.err("audio", "no audio samples")
        if a.audio_seq and aud:
            def seq(n):
                l = (n * 1237) & 0xFFFF
                r = (n * 4813 + 77) & 0xFFFF
                return (l - 65536 if l & 0x8000 else l, r - 65536 if r & 0x8000 else r)
            # find n for the first sample: L(n) = 1237 n mod 2^16, and
            # 1237 is odd, so n mod 2^16 is unique
            inv = pow(1237, -1, 65536)
            n0 = ((aud[0][0] & 0xFFFF) * inv) & 0xFFFF
            k = a.audio_rep
            # the first frame may be partly sent before the capture began
            skip = 0
            while skip < k and skip < len(aud) and aud[skip] == aud[0]:
                skip += 1
            off = k - skip
            bad = 0
            for i, smp in enumerate(aud):
                want = seq(n0 + (i + off) // k)
                if smp != want:
                    bad += 1
                    if bad <= 3:
                        rep.err("audio", "sample %d is %r, want %r" % (i, smp, want))
            print("  audio: %d samples, sequence from n = %d, x%d, %s" % (
                len(aud), n0, k, "intact" if not bad else "%d wrong" % bad))
        if a.audio_hz and aud:
            secs = len(chars) / 25.2e6
            got = len(aud) / secs
            print("  audio rate: %.1f Hz measured over %.1f ms (want %.1f)" % (got, secs * 1000, a.audio_hz))
            if abs(got - a.audio_hz) > a.audio_hz * 0.005:
                rep.err("audio", "rate %.1f Hz, want %.1f" % (got, a.audio_hz))
            for (nn, cts) in summ["acr"]:
                fs = 25.2e6 * nn / (128.0 * cts)
                print("  ACR says %.2f Hz" % fs)
                if abs(fs - a.audio_hz) > a.audio_hz * 0.0005:
                    rep.err("audio", "N/CTS %d/%d say %.2f Hz, want %.1f" % (nn, cts, fs, a.audio_hz))
        if summ["cs_blocks"]:
            print("  channel-status blocks: %d of 192 frames" % len(summ["cs_blocks"]))
    if a.write_sha256:
        import hashlib
        h = hashlib.sha256("".join("%03x%03x%03x" % c for c in chars).encode()).hexdigest()
        with open(a.write_sha256, "w") as f:
            f.write("%s  %s\n" % (h, os.path.basename(a.capture)))
        print("  wrote sha256 %s to %s" % (h, a.write_sha256))
    if a.sha256:
        import hashlib
        h = hashlib.sha256("".join("%03x%03x%03x" % c for c in chars).encode()).hexdigest()
        want = open(a.sha256).read().split()[0]
        print("  sha256 %s (%s)" % (h, "matches" if h == want else "golden " + want))
        if h != want:
            rep.err("sha256", "capture differs from the golden")
    for e in rep.errors:
        print("ERROR", e)
    print("PASS" if rep.ok() else "FAIL (%d errors)" % len(rep.errors))
    return 0 if rep.ok() else 1


# What rtl/gpu/ddmi_island.v sends, line by line, for --model: slot 0
# null (audio from phase 3), slot 1 GCP / AVI / SPD on vc 10 / 11 / 12.
def schedule_round2(f, vc):
    slot1 = {10: gcp(clear_avmute=True), 11: avi_infoframe(), 12: spd_infoframe()}
    return [null_packet(), slot1.get(vc, null_packet())]


SCHEDULES = {"dvi": None, "round2": schedule_round2}


# Pixel patterns shared with rtl/gpu/bench/tb_ddmi_capture.v: (r, g, b)
# 8-bit as the encoder is driven. Keep in step with the bench.
PATTERNS = {
    "xor": lambda x, y, f: ((x ^ y) & 0xFF, (x + 3 * y) & 0xFF, (x * 7 + y) & 0xFF),
}


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
