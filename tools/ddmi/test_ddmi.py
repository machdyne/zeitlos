#!/usr/bin/env python3
"""
Self-test for tools/ddmi/ddmi.py: the model against independent
references, and the checker against streams that are right and streams
that are deliberately wrong.

    python3 tools/ddmi/test_ddmi.py

The references are the tables of two open-source TMDS/HDMI cores
(hdl-util/hdmi, Apache-2.0/MIT; shuichitakano/pico_lib, for its BCH
lookup table), copied here as data so the test needs neither.
"""
import os, random, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ddmi as D

fails = 0
checks = 0


def check(cond, msg):
    global fails, checks
    checks += 1
    if not cond:
        fails += 1
        print("FAIL", msg)


# 1. BCH: pico_lib's byte table is "parity after one byte from zero".
PICO_BCH = [0x00, 0xd9, 0xb5, 0x6c, 0x6d, 0xb4, 0xd8, 0x01,
            0xda, 0x03, 0x6f, 0xb6, 0xb7, 0x6e, 0x02, 0xdb,
            0xb3, 0x6a, 0x06, 0xdf, 0xde, 0x07, 0x6b, 0xb2]
for x, want in enumerate(PICO_BCH):
    check(D.bch_ecc([x]) == want, "bch of %02x = %02x want %02x" % (x, D.bch_ecc([x]), want))
# linearity: the code is linear, so parity(a ^ b) = parity(a) ^ parity(b)
rnd = random.Random(1)
for _ in range(200):
    a = [rnd.randrange(256) for _ in range(7)]
    b = [rnd.randrange(256) for _ in range(7)]
    check(D.bch_ecc([x ^ y for x, y in zip(a, b)]) == D.bch_ecc(a) ^ D.bch_ecc(b), "bch linear")

# 2. TERC4 and the control tokens: distinct, and none is a control token
check(len(set(D.TERC4)) == 16, "TERC4 codes distinct")
check(not (set(D.TERC4) & set(D.CTRL.values())), "TERC4 and control tokens disjoint")
check(D.DATA_GB_12 not in D.TERC4, "data guard band is not a TERC4 code")
check(D.data_gb0(0, 0) == 0b1010001110 and D.data_gb0(1, 1) == 0b1011000011, "ch0 data guard band")

# 3. TMDS video: every byte round-trips at every disparity, and the
#    running disparity stays bounded
e = D.TmdsEncoder()
worst = 0
for k in range(20000):
    d = rnd.randrange(256)
    q = e.video(d)
    check(D.tmds_decode(q) == d, "tmds round trip %02x" % d) if k < 2000 else None
    if D.tmds_decode(q) != d:
        check(False, "tmds round trip %02x" % d)
    worst = max(worst, abs(e.cnt))
    # no video character is a control token (7 transitions vs <= 5)
    if q in D.CTRL_INV:
        check(False, "video char %03x is a control token" % q)
check(worst <= 10, "running disparity bounded (%d)" % worst)

# 4. A DVI frame decodes back to its pixels and has no islands
pat = D.PATTERNS["xor"]
dvi = D.encode_frames(lambda x, y: pat(x, y, 0), None, frames=2)
rep = D.decode(dvi)
check(rep.ok(), "clean DVI decodes: %s" % rep.errors[:3])
check(rep.mode == "dvi" and not rep.packets, "DVI has no packets")
full = [fr for fr in rep.frames if len(fr) == 480]
check(full and all(len(r) == 640 for r in full[0]), "DVI frame is 640x480")
check(full and all(full[0][y][x] == pat(x, y, 0) for y in range(0, 480, 37) for x in range(0, 640, 13)),
      "DVI pixels")

# 5. An HDMI-mode stream with every packet Zeitlos sends
cs = D.channel_status_bits(2)
samples = [(rnd.randrange(-32768, 32768), rnd.randrange(-32768, 32768)) for _ in range(2000)]
state = {"n": 0}
acr = D.acr(6144, 25200)
avi = D.avi_infoframe()
aif = D.audio_infoframe()
spd = D.spd_infoframe()


def packets(f, vc):
    # 48 kHz at 31.5 kHz lines: 1 or 2 samples per line
    want = (int((f * 525 + vc + 1) * 48000 / 31500) - int((f * 525 + vc) * 48000 / 31500))
    pk = []
    if want:
        n0 = state["n"]
        pk.append(D.audio_sample_packet(samples[n0:n0 + want], n0, lambda i: cs[i]))
        state["n"] += want
    slot2 = [acr, avi, aif, spd][vc % 4] if vc % 16 < 4 else None
    if slot2:
        pk.append(slot2)
    return pk


hd = D.encode_frames(lambda x, y: pat(x, y, 0), packets, frames=2)
rep = D.decode(hd)
summ = D.check_packets(rep)
check(rep.ok(), "clean HDMI stream decodes: %s" % rep.errors[:3])
check(rep.mode == "hdmi", "mode hdmi")
check(summ["acr"] == {(6144, 25200)}, "ACR N/CTS %s" % summ["acr"])
check(summ["infoframes"].get("AVI") == (2, avi.sb[0][1:] + avi.sb[1][:7]), "AVI recovered")
check("audio-IF" in summ["infoframes"] and "SPD" in summ["infoframes"], "audio IF and SPD recovered")
got = summ["audio"]
check(got == samples[:len(got)] and len(got) > 1500, "audio samples bit-exact (%d)" % len(got))
check(summ["cs_blocks"] and D.cs_fields(summ["cs_blocks"][0])["fs_code"] == 2, "channel status 48 kHz")
check(rep.ext_ctl_frames >= 1, "extended control period present")
full = [fr for fr in rep.frames if len(fr) == 480]
check(full and full[0][100][200] == pat(200, 100, 0), "HDMI pixels")
avi_bytes = avi.sb[0][1:6]
check(avi_bytes == [0x12, 0x18, 0x88, 0x01, 0x00], "AVI PB1..5 = %s" % avi_bytes)


# 6. The checker catches what it should
def expect_error(mut, what):
    s = list(hd)
    mut(s)
    r = D.decode(s)
    D.check_packets(r)
    check(not r.ok(), "checker misses: " + what)

# find the first island's first packet character
first_pkt = next(i for i in range(len(hd)) if hd[i][1] == D.DATA_GB_12) + 2
expect_error(lambda s: s.__setitem__(first_pkt + 5, (s[first_pkt + 5][0], D.TERC4[(D.TERC4_INV[s[first_pkt + 5][1]] ^ 1)], s[first_pkt + 5][2])),
             "a flipped subpacket bit (BCH)")
expect_error(lambda s: s.__setitem__(first_pkt + 7, (D.TERC4[D.TERC4_INV[s[first_pkt + 7][0]] ^ 4], s[first_pkt + 7][1], s[first_pkt + 7][2])),
             "a flipped header bit (BCH)")
expect_error(lambda s: s.__setitem__(first_pkt, (D.TERC4[D.TERC4_INV[s[first_pkt][0]] | 8], s[first_pkt][1], s[first_pkt][2])),
             "ch0 bit 3 set on the first character")
pre = first_pkt - 3
expect_error(lambda s: s.__setitem__(pre, (s[pre][0], D.CTRL[0], D.CTRL[0])), "a 7-character preamble")
expect_error(lambda s: s.__setitem__(first_pkt - 1, (s[first_pkt - 1][0], D.CTRL[0], D.CTRL[0])), "a missing guard band")

# 7. the gateware's packet constants are the model's packets
import gen_packets
rtl = open(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "..", "..", "rtl", "gpu", "ddmi_island.v")).read()
for name, p, what in gen_packets.PACKETS:
    line = "localparam [247:0] %s = 248'h%062x;" % (name, gen_packets.vec(p))
    check(line in rtl, "rtl/gpu/ddmi_island.v %s differs from the model (run gen_packets.py)" % name)

print("test_ddmi: %d checks, %d failures" % (checks, fails))
sys.exit(1 if fails else 0)
