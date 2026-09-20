#!/usr/bin/env python3
"""
Decode a USB wire capture from the Zeitlos built-in logic probe.

    usbcap            (on the board: arm)
    ...plug the device in, wait for "capture frozen"...
    usbcapd           (on the board: print it)

Save the console output of `usbcapd` to a file and run:

    tools/usbcap.py capture.txt

Input format (rtl/probe.v, sw/os/usb/usbh.c z_usbh_cap_dump):
  - optional header:   usbcap: addr N state S stage N status S speed low|full
  - then lines         XXXX: wwwwwwww wwwwwwww ... (eight 32-bit words)
Each word holds sixteen samples, two bits each, OLDEST in the low bits;
each sample is {D+, D-}. Samples are 48 MHz, one per 20.833 ns.

The probe watches root port 0, where a hub is. Everything on that wire
uses full-speed polarity -- a low-speed device behind a hub is repeated
upstream with J = D+ high -- so J is D+ high throughout.

Output: one line per packet -- start time, gap since the previous
packet, rate, PID, contents, CRC -- and anything the wire should never
show: SE1 (two drivers at once), glitches shorter than half a bit, and
bit-stuffing violations.
"""

import re
import sys

NS_PER_SAMPLE = 1000.0 / 48.0      # 48 MHz
FS_BIT = 4.0                        # samples per bit
LS_BIT = 32.0

J, K, SE0, SE1 = "J", "K", "0", "1"

PIDS = {
    0b0001: "OUT", 0b1001: "IN", 0b0101: "SOF", 0b1101: "SETUP",
    0b0011: "DATA0", 0b1011: "DATA1", 0b0111: "DATA2", 0b1111: "MDATA",
    0b0010: "ACK", 0b1010: "NAK", 0b1110: "STALL", 0b0110: "NYET",
    0b1100: "PRE",
}
TOKENS = ("OUT", "IN", "SOF", "SETUP")
DATAS = ("DATA0", "DATA1", "DATA2", "MDATA")


def parse(lines):
    header = None
    words = []
    for ln in lines:
        m = re.search(r"usbcap: (addr .*)", ln)
        if m:
            header = m.group(1).strip()
        # Placed by the index printed at the start of the line, so a
        # console message landing in the middle of the dump -- or a line
        # cut short -- cannot shift the data. Missing words are reported.
        for m in re.finditer(r"(?:^|\s)([0-9a-fA-F]{4,8}):((?:\s+[0-9a-fA-F]{8})+)", ln):
            base = int(m.group(1), 16)
            for n, w in enumerate(m.group(2).split()):
                words.append((base + n, int(w, 16)))
    if not words:
        return header, []
    got = dict(words)
    top = max(got) + 1
    missing = [i for i in range(top) if i not in got]
    if missing:
        print("WARNING: %d capture word(s) missing, first at %d -- "
              "treated as idle J" % (len(missing), missing[0]))
    # idle J in every sample: D+ high, D- low = binary 10 per pair
    return header, [got.get(i, 0xAAAAAAAA) for i in range(top)]


def samples(words):
    out = []
    for w in words:
        for k in range(16):
            s = (w >> (2 * k)) & 3
            dp, dm = (s >> 1) & 1, s & 1
            out.append({(1, 0): J, (0, 1): K, (0, 0): SE0, (1, 1): SE1}[(dp, dm)])
    return out


def runs(smp):
    """[(state, start_sample, length)]"""
    r = []
    i = 0
    while i < len(smp):
        j = i
        while j < len(smp) and smp[j] == smp[i]:
            j += 1
        r.append((smp[i], i, j - i))
        i = j
    return r


def crc5_ok(bits11, crc_bits):
    c = 0x1F
    for b in bits11:
        c = (c >> 1) ^ (0x14 if (b ^ (c & 1)) else 0)
    want = [(~c >> i) & 1 for i in range(5)]
    return want == crc_bits


def crc16_residual(bits):
    c = 0xFFFF
    for b in bits:
        c = (c >> 1) ^ (0xA001 if (b ^ (c & 1)) else 0)
    return c


def us(sample):
    return sample * NS_PER_SAMPLE / 1000.0


def decode(smp):
    rs = runs(smp)
    notes = []
    for st, start, n in rs:
        if st == SE1:
            # One sample between J and K is the two lines crossing a few
            # ns apart -- normal on slow low-speed edges repeated by a
            # hub. Longer is two drivers at once.
            if n <= 1:
                notes.append(f"{us(start):9.3f} us  SE1 for 1 sample -- edge crossover, harmless")
            else:
                notes.append(f"{us(start):9.3f} us  SE1 for {n} samples -- TWO DRIVERS at once")

    pkts = []
    i = 0
    last_end = None
    while i < len(rs):
        st, start, n = rs[i]
        # a packet starts with K after idle J (or after a PRE's gap)
        if st != K:
            i += 1
            continue
        bitlen = FS_BIT if n < 12 else LS_BIT
        rate = "FS" if bitlen == FS_BIT else "LS"
        # NRZI decode run by run: a change of state is a 0, then each
        # further bit time in the same state is a 1.
        bits = []
        prev = J
        eop_len = None
        stuff_err = False
        glitch = False
        ones = 0
        k = i
        pre_end = None
        while k < len(rs):
            s2, st2, n2 = rs[k]
            if s2 == SE0:
                eop_len = n2
                break
            if s2 == SE1:
                break
            nb = int(round(n2 / bitlen))
            if nb == 0:
                glitch = True
                nb = 1
            raw = [0 if s2 != prev else 1] + [1] * (nb - 1)
            prev = s2
            for b in raw:
                if ones == 6:
                    # stuffed zero expected
                    if b != 0:
                        stuff_err = True
                    ones = 0
                    continue
                bits.append(b)
                ones = ones + 1 if b else 0
            # A PRE is SYNC + PID at full speed and then idle J -- no
            # EOP. Stop after its PID; the low-speed packet follows.
            if rate == "FS" and len(bits) >= 16:
                pid = sum(bits[8 + x] << x for x in range(4))
                if pid == 0b1100:
                    pre_end = st2 + n2
                    # the PID's last bit may end inside this run: cut it
                    bits = bits[:16]
                    break
            k += 1
        # drop SYNC: bits up to and including the first "1" that ends
        # the KJKJKJKK pattern -- SYNC is 00000001 in NRZI-decoded form
        try:
            first_one = bits.index(1)
            payload = bits[first_one + 1:]
        except ValueError:
            payload = []
        gap = (start - last_end) if last_end is not None else None
        cut = (pre_end is None and eop_len is None and k >= len(rs))
        p = {"t": us(start), "gap": (us(gap) if gap is not None else None),
             "rate": rate, "bits": payload, "eop": eop_len,
             "stuff_err": stuff_err, "glitch": glitch and not cut,
             "cut": cut}
        if pre_end is not None:
            p["pre"] = True
            last_end = pre_end
            # continue after the PRE's run
            while k < len(rs) and rs[k][1] < pre_end:
                k += 1
            i = k
        else:
            last_end = (rs[k][1] + rs[k][2]) if k < len(rs) else len(smp)
            i = k + 1
        pkts.append(p)
    return pkts, notes


def describe(p):
    b = p["bits"]
    if len(b) < 8:
        return "(runt: %d bits)" % len(b)
    pid = sum(b[x] << x for x in range(4))
    chk = sum(b[4 + x] << x for x in range(4))
    name = PIDS.get(pid, "PID?%x" % pid)
    s = name
    if chk != (~pid & 0xF):
        s += " [PID CHECK BAD %x/%x]" % (pid, chk)
    rest = b[8:]
    if name in TOKENS:
        if len(rest) >= 16:
            f = rest[:11]
            addr = sum(f[x] << x for x in range(7))
            ep = sum(f[7 + x] << x for x in range(4))
            ok = crc5_ok(f, rest[11:16])
            if name == "SOF":
                s += " frame %d" % sum(f[x] << x for x in range(11))
            else:
                s += " addr %d ep %d" % (addr, ep)
            s += "" if ok else " [CRC5 BAD]"
            if len(rest) > 16:
                s += " [+%d extra bits]" % (len(rest) - 16)
        else:
            s += " [short token: %d bits]" % len(rest)
    elif name in DATAS:
        nbytes = len(rest) // 8
        data = [sum(rest[8 * y + x] << x for x in range(8)) for y in range(nbytes)]
        payload = data[:-2] if nbytes >= 2 else data
        s += " %d bytes: %s" % (len(payload), " ".join("%02x" % v for v in payload))
        res = crc16_residual(rest[:nbytes * 8])
        s += "" if res == 0xB001 else " [CRC16 BAD]"
        if len(rest) % 8:
            s += " [+%d stray bits]" % (len(rest) % 8)
    elif name == "PRE":
        pass
    elif len(rest):
        s += " [+%d extra bits]" % len(rest)
    return s


def main():
    src = open(sys.argv[1]) if len(sys.argv) > 1 else sys.stdin
    header, words = parse(src.readlines())
    if not words:
        print("no capture words found")
        return 1
    if header:
        print("failing transaction:", header)
    smp = samples(words)
    print("%d samples, %.1f us" % (len(smp), us(len(smp))))
    pkts, notes = decode(smp)
    # The failing transaction is the LAST token to the header's address:
    # the driver keeps a window recording across transactions, so a
    # capture also holds what came before it.
    # Marked by the PID in the header when there is one: the stage number
    # alone does not say which transaction failed.
    fail_i = None
    m = re.match(r"addr (\d+)(?: pid (\w+))?", header or "")
    if m:
        want = int(m.group(1))
        pids = m.group(2) or "IN|OUT|SETUP"
        for i, p in enumerate(pkts):
            d = describe(p)
            if re.match(r"(%s) addr %d ep" % (pids, want), d):
                fail_i = i
    for i, p in enumerate(pkts):
        gap = "      -   " if p["gap"] is None else "%8.3f  " % p["gap"]
        eop = ""
        if p.get("pre"):
            eop = "  (no EOP: preamble)"
        elif p["eop"] is not None:
            bl = FS_BIT if p["rate"] == "FS" else LS_BIT
            eop = "  EOP %.1f bits" % (p["eop"] / bl)
        flags = ""
        if p.get("cut"):
            eop = "  (cut off by the end of the capture)"
        if p["stuff_err"]:
            flags += " [BIT STUFF VIOLATION]"
        if p["glitch"]:
            flags += " [GLITCH]"
        mark = "   <-- the failing transaction" if i == fail_i else ""
        print("%9.3f us  gap %s %s  %s%s%s%s" % (p["t"], gap, p["rate"],
                                               describe(p), eop, flags, mark))
    if notes:
        print()
        for n in notes:
            print(n)
    return 0


if __name__ == "__main__":
    sys.exit(main())
