#!/usr/bin/env python3
# Zeitlos -- the 8b/10b tables behind zlink (rtl/gpio_stream.v).
#
#   python3 tools/gen_8b10b.py --verilog   the decode/encode case tables
#                                           pasted into rtl/gpio_stream.v
#   python3 tools/gen_8b10b.py --vectors   rtl/tests/8b10b_vectors.txt
#   python3 tools/gen_8b10b.py --update rtl/gpio_stream.v
#                                           rewrite those tables in place
#   python3 tools/gen_8b10b.py --check     property checks only (default)
#
# The code is the standard IBM 8b/10b (Widmer and Franaszek, 1983), the
# one Fibre Channel, Gigabit Ethernet, SATA and PCIe 1/2 use. Nothing
# here is invented; the point of this file is that the tables in the RTL
# are GENERATED from the published sub-block tables below rather than
# typed, and that the result is checked against properties the code is
# known to have before a vector is written:
#
#   - every symbol is 10 bits with disparity 0 or +-2, and the running
#     disparity alternates correctly (never leaves {-1, +1});
#   - no run of more than five equal bits, inside a symbol or across
#     any boundary of any two consecutive symbols;
#   - the comma (0011111 / 1100000) appears ONLY inside K28.1, K28.5
#     and K28.7, and at a symbol boundary -- never straddling two
#     symbols. This is what lets the receiver align on K28.5;
#   - every symbol decodes back to the byte and K flag it came from,
#     and no two (K, byte) pairs share a symbol.
#
# Bit order everywhere is TRANSMISSION order, 'a' first: abcdei then
# fghj. In the RTL a symbol is held with 'a' in bit 0, so the string
# "abcdeifghj" maps to sym[0]..sym[9].

import sys

# 5b/6b: x -> (RD- code, RD+ code), written abcdei.
T6 = {
    0: ("100111", "011000"),  1: ("011101", "100010"),
    2: ("101101", "010010"),  3: ("110001", "110001"),
    4: ("110101", "001010"),  5: ("101001", "101001"),
    6: ("011001", "011001"),  7: ("111000", "000111"),
    8: ("111001", "000110"),  9: ("100101", "100101"),
    10: ("010101", "010101"), 11: ("110100", "110100"),
    12: ("001101", "001101"), 13: ("101100", "101100"),
    14: ("011100", "011100"), 15: ("010111", "101000"),
    16: ("011011", "100100"), 17: ("100011", "100011"),
    18: ("010011", "010011"), 19: ("110010", "110010"),
    20: ("001011", "001011"), 21: ("101010", "101010"),
    22: ("011010", "011010"), 23: ("111010", "000101"),
    24: ("110011", "001100"), 25: ("100110", "100110"),
    26: ("010110", "010110"), 27: ("110110", "001001"),
    28: ("001110", "001110"), 29: ("101110", "010001"),
    30: ("011110", "100001"), 31: ("101011", "010100"),
}
K28_6 = ("001111", "110000")

# 3b/4b: y -> (RD- code, RD+ code), written fghj. 7 has two forms.
T4 = {
    0: ("1011", "0100"), 1: ("1001", "1001"), 2: ("0101", "0101"),
    3: ("1100", "0011"), 4: ("1101", "0010"), 5: ("1010", "1010"),
    6: ("0110", "0110"), 7: ("1110", "0001"),
}
A7 = ("0111", "1000")

# K28.y's 4-bit half, by the running disparity at the START of the
# symbol (K28's 6-bit half is unbalanced, so this is not the same as the
# disparity the 4-bit half sees).
K28_4 = {
    0: ("0100", "1011"), 1: ("1001", "0110"), 2: ("0101", "1010"),
    3: ("0011", "1100"), 4: ("0010", "1101"), 5: ("1010", "0101"),
    6: ("0110", "1001"), 7: ("1000", "0111"),
}

# The twelve control symbols: K28.0-7 and K23/27/29/30.7.
K_CODES = [(28, y) for y in range(8)] + [(x, 7) for x in (23, 27, 29, 30)]


def disp(s):
    return 2 * s.count("1") - len(s)


def encode(byte, k, rd):
    """(byte, K flag, running disparity -1/+1) -> (10-char string, rd')."""
    x, y = byte & 31, byte >> 5
    neg = rd < 0
    if k:
        if (x, y) not in K_CODES:
            raise ValueError("K.%d.%d is not a control symbol" % (x, y))
        if x == 28:
            s6 = K28_6[0] if neg else K28_6[1]
            s4 = K28_4[y][0] if neg else K28_4[y][1]
            sym = s6 + s4
            return sym, rd + disp(sym) if disp(sym) else rd
        s6 = T6[x][0] if neg else T6[x][1]
        rd1 = rd if disp(s6) == 0 else -rd
        s4 = A7[0] if rd1 < 0 else A7[1]
        rd2 = rd1 if disp(s4) == 0 else -rd1
        return s6 + s4, rd2
    s6 = T6[x][0] if neg else T6[x][1]
    rd1 = rd if disp(s6) == 0 else -rd
    if y == 7:
        alt = (rd1 < 0 and x in (17, 18, 20)) or (rd1 > 0 and x in (11, 13, 14))
        s4 = (A7 if alt else T4[7])[0 if rd1 < 0 else 1]
    else:
        s4 = T4[y][0 if rd1 < 0 else 1]
    rd2 = rd1 if disp(s4) == 0 else -rd1
    return s6 + s4, rd2


def all_symbols():
    """Every (rd_in, k, byte) -> (sym string, rd_out)."""
    out = []
    for rd in (-1, 1):
        for b in range(256):
            out.append((rd, 0, b) + encode(b, 0, rd))
        for (x, y) in K_CODES:
            b = (y << 5) | x
            out.append((rd, 1, b) + encode(b, 1, rd))
    return out


def check():
    syms = all_symbols()
    seen = {}
    for rd, k, b, s, rd2 in syms:
        assert len(s) == 10, s
        d = disp(s)
        assert d in (-2, 0, 2), (k, b, s)
        # disparity alternation: a symbol from RD- is never negative
        assert (rd < 0 and d >= 0) or (rd > 0 and d <= 0), (rd, k, b, s)
        assert rd2 in (-1, 1)
        assert "111111" not in s and "000000" not in s, (k, b, s)
        key = s
        assert key not in seen or seen[key] == (k, b), ("collision", s)
        seen[key] = (k, b)
    # well-known symbols, from the standard rather than from this file
    assert encode(0xBC, 1, -1)[0] == "0011111010"     # K28.5 RD-
    assert encode(0xBC, 1, +1)[0] == "1100000101"     # K28.5 RD+
    assert encode(0x00, 0, -1)[0] == "1001110100"     # D0.0 RD-
    assert encode(0xB5, 0, -1)[0] == "1010101010"     # D21.5
    assert encode(0xFB, 1, -1)[0] == "1101101000"     # K27.7 RD-
    assert encode(0xFD, 1, -1)[0] == "1011101000"     # K29.7 RD-
    # run length and comma position across every pair of symbols
    by_rd = {-1: [], 1: []}
    for rd, k, b, s, rd2 in syms:
        by_rd[rd].append((s, rd2, k, b))
    comma = ("0011111", "1100000")
    for s1, r1, k1, b1 in by_rd[-1] + by_rd[1]:
        # K28.7 is the one exception the standard documents: its tail
        # and the next symbol's head can form a comma one place to the
        # right, which would mis-align a receiver. zlink never sends
        # K28.7 (docs/zlink.md lists the control symbols it uses), so
        # it is excluded from the check rather than from the table.
        if k1 and b1 == 0xFC:
            continue
        for s2, r2, k2, b2 in by_rd[r1]:
            pair = s1 + s2
            assert "111111" not in pair and "000000" not in pair, (s1, s2)
            for c in comma:
                i = pair.find(c)
                while i >= 0:
                    # inside a symbol at bit 0 is the only legal place
                    assert i in (0, 10), ("comma straddles", s1, s2, i)
                    who = (k1, b1) if i == 0 else (k2, b2)
                    assert who[0] == 1 and (who[1] & 31) == 28 and \
                        (who[1] >> 5) in (1, 5, 7), ("comma in", who)
                    i = pair.find(c, i + 1)
    return syms


def rtl_sym(s):
    """'abcdeifghj' -> integer with 'a' in bit 0."""
    v = 0
    for i, ch in enumerate(s):
        if ch == "1":
            v |= 1 << i
    return v


def verilog():
    """The case tables rtl/gpio_stream.v uses, indexed in RTL bit order
    (a in bit 0). Encoding is split into the 5b/6b and 3b/4b halves the
    same way the standard is, which is what keeps it to a few dozen
    LUT4; decoding is the inverse of each half."""
    def b6(s):
        return "6'b" + "".join(reversed(s))  # 'a' ends up as bit 0

    def b4(s):
        return "4'b" + "".join(reversed(s))

    L = []
    L.append("\t// -- generated by tools/gen_8b10b.py --verilog; do not edit --")
    L.append("\t// 5b/6b, RD- form (bit 0 = a). bit 6 set: unbalanced, so the")
    L.append("\t// RD+ form is the complement and the disparity flips. D.07 is")
    L.append("\t// balanced but still has two forms (bit 7).")
    L.append("\tfunction [7:0] enc6;")
    L.append("\t\tinput [4:0] x;")
    L.append("\t\tcase (x)")
    for x in range(32):
        neg, pos = T6[x]
        unbal = disp(neg) != 0
        two = (not unbal) and neg != pos
        L.append("\t\t\t5'd%d: enc6 = {1'b%d, 1'b%d, %s};" %
                 (x, 1 if two else 0, 1 if unbal else 0, b6(neg)))
    L.append("\t\tendcase")
    L.append("\tendfunction")
    L.append("")
    L.append("\t// 3b/4b for data, RD- form (bit 0 = f), same flag layout as")
    L.append("\t// enc6. y = 7 here is the primary form; the A7 alternate is")
    L.append("\t// chosen by the caller.")
    L.append("\tfunction [5:0] enc4;")
    L.append("\t\tinput [2:0] y;")
    L.append("\t\tcase (y)")
    for y in range(8):
        neg, pos = T4[y]
        unbal = disp(neg) != 0
        two = (not unbal) and neg != pos
        L.append("\t\t\t3'd%d: enc4 = {1'b%d, 1'b%d, %s};" %
                 (y, 1 if two else 0, 1 if unbal else 0, b4(neg)))
    L.append("\t\tendcase")
    L.append("\tendfunction")
    L.append("")
    L.append("\t// 3b/4b for K28.y when the symbol STARTS at RD- (after the")
    L.append("\t// 001111 half); bit 4: unbalanced. From RD+ the whole")
    L.append("\t// symbol is the complement.")
    L.append("\tfunction [4:0] enc4k28;")
    L.append("\t\tinput [2:0] y;")
    L.append("\t\tcase (y)")
    for y in range(8):
        neg = K28_4[y][0]
        L.append("\t\t\t3'd%d: enc4k28 = {1'b%d, %s};" %
                 (y, 1 if disp(neg) != 0 else 0, b4(neg)))
    L.append("\t\tendcase")
    L.append("\tendfunction")
    L.append("")
    L.append("\t// 6b/5b decode: {valid, x}. Both RD forms map to the same x.")
    L.append("\tfunction [5:0] dec6;")
    L.append("\t\tinput [5:0] s;")
    L.append("\t\tcase (s)")
    m6 = {}
    for x in range(32):
        for s in T6[x]:
            m6[rtl_sym(s) & 63] = x
    for s in sorted(m6):
        L.append("\t\t\t6'd%d: dec6 = {1'b1, 5'd%d};" % (s, m6[s]))
    L.append("\t\t\tdefault: dec6 = 6'd0;")
    L.append("\t\tendcase")
    L.append("\tendfunction")
    L.append("")
    L.append("\t// 4b/3b decode for data: {valid, y}. The A7 forms decode to 7.")
    L.append("\tfunction [3:0] dec4;")
    L.append("\t\tinput [3:0] s;")
    L.append("\t\tcase (s)")
    m4 = {}
    for y in range(8):
        for s in T4[y]:
            m4[rtl_sym(s)] = y
    for s in A7:
        m4[rtl_sym(s)] = 7
    for s in sorted(m4):
        L.append("\t\t\t4'd%d: dec4 = {1'b1, 3'd%d};" % (s, m4[s]))
    L.append("\t\t\tdefault: dec4 = 4'd0;")
    L.append("\t\tendcase")
    L.append("\tendfunction")
    L.append("")
    L.append("\t// 4b/3b decode for K28.y, from the RD- form of the symbol")
    L.append("\t// (001111 ....); the RD+ form is the complement of the whole")
    L.append("\t// symbol, so the caller complements first.")
    L.append("\tfunction [3:0] dec4k28;")
    L.append("\t\tinput [3:0] s;")
    L.append("\t\tcase (s)")
    for y in range(8):
        L.append("\t\t\t%s: dec4k28 = {1'b1, 3'd%d};" % (b4(K28_4[y][0]), y))
    L.append("\t\t\tdefault: dec4k28 = 4'd0;")
    L.append("\t\tendcase")
    L.append("\tendfunction")
    L.append("\t// -- end generated --")
    return "\n".join(L)


def vectors(syms):
    """One line per symbol: rd_in(0=-,1=+) k byte sym(hex, a=bit0) rd_out."""
    L = [str(len(syms))]
    for rd, k, b, s, rd2 in syms:
        L.append("%d %d %02x %03x %d" % (0 if rd < 0 else 1, k, b,
                                          rtl_sym(s), 0 if rd2 < 0 else 1))
    return "\n".join(L)


def update(path):
    """Replace the generated block in an RTL file in place."""
    begin = "\t// -- generated by tools/gen_8b10b.py --verilog; do not edit --"
    end = "\t// -- end generated --"
    with open(path) as f:
        text = f.read()
    i = text.index(begin)
    j = text.index(end, i) + len(end)
    new = text[:i] + verilog() + text[j:]
    if new != text:
        with open(path, "w") as f:
            f.write(new)
        print("%s: tables updated" % path)
    else:
        print("%s: tables already current" % path)


if __name__ == "__main__":
    syms = check()
    if "--update" in sys.argv:
        update(sys.argv[sys.argv.index("--update") + 1])
    elif "--verilog" in sys.argv:
        print(verilog())
    elif "--vectors" in sys.argv:
        print(vectors(syms))
    else:
        print("8b/10b: %d symbols, all properties hold" % len(syms))
