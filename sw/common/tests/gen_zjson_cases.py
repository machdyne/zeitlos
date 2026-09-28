#!/usr/bin/env python3
# Regenerates zjson_cases.txt for test_zjson.c.
#
#   python3 gen_zjson_cases.py > zjson_cases.txt
#
# The verdict on every case comes from Python's own json module plus
# the subset's extra rules (below) -- a reference independent of
# zjson.c. For an accepted case, also the value as json.dumps writes it,
# which the C test must reproduce from what it parsed.
import json, random, sys
random.seed(20260927)
MAX = 2**53

class Refuse(Exception): pass

def no_float(s): raise Refuse("float")
def no_const(s): raise Refuse("constant")
def int_hook(s):
    if s == "-0": raise Refuse("-0")
    v = int(s)
    if abs(v) > MAX: raise Refuse("range")
    return v
def pairs(ps):
    if len(ps) > 256: raise Refuse("keys")
    keys = [k for k, _ in ps]
    if len(set(keys)) != len(keys): raise Refuse("dup")
    return dict(ps)

def check_strings(v, depth=0):
    if depth > 16: raise Refuse("depth")
    if isinstance(v, str):
        if "\x00" in v: raise Refuse("nul")
        v.encode("utf-8")			# a lone surrogate fails here
    elif isinstance(v, dict):
        for k, x in v.items():
            check_strings(k, depth); check_strings(x, depth + 1)
    elif isinstance(v, list):
        for x in v: check_strings(x, depth + 1)

def depth_of(v):
    if isinstance(v, dict): return 1 + max([depth_of(x) for x in v.values()] + [0])
    if isinstance(v, list): return 1 + max([depth_of(x) for x in v] + [0])
    return 0

def verdict(b):
    if b.startswith(b"\xef\xbb\xbf"): return None
    try:
        text = b.decode("utf-8")
        v = json.loads(text, parse_float=no_float, parse_constant=no_const,
                       parse_int=int_hook, object_pairs_hook=pairs, strict=True)
        check_strings(v)
        if depth_of(v) > 16: raise Refuse("depth")
        return json.dumps(v, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    except (Refuse, ValueError, UnicodeError, RecursionError):
        return None

def rstr():
    out = []
    for _ in range(random.randrange(0, 8)):
        r = random.random()
        if r < 0.5: out.append(random.choice("abcXYZ019 _-/"))
        elif r < 0.6: out.append(random.choice(['\\"', '\\\\', '\\/', '\\b', '\\f', '\\n', '\\r', '\\t']))
        elif r < 0.7: out.append("\\u%04x" % random.choice([0x41, 0xe9, 0x20ac, 0x7f, 0x1f, random.randrange(0x20, 0xd7ff)]))
        elif r < 0.75: out.append("\\ud83d\\ude00")
        elif r < 0.9: out.append(random.choice("éü€漢字🙂ß"))
        else: out.append(random.choice(["\\u0000", "\\ud800", "\\udc00", "\x01", "\\x", "\\u12"]))
    return '"' + "".join(out) + '"'

def rval(d=0):
    r = random.random()
    if d > 5 or r < 0.3: return rstr()
    if r < 0.45:
        return random.choice(["0", "-1", "7", str(MAX), str(-MAX), str(MAX + 1), "-0", "01", "1.5", "1e3", str(random.randrange(-10**6, 10**6))])
    if r < 0.5: return random.choice(["true", "false", "null"])
    if r < 0.75:
        n = random.randrange(0, 5)
        return "[" + ",".join(rval(d + 1) for _ in range(n)) + "]"
    n = random.randrange(0, 5)
    keys = [rstr() for _ in range(n)]
    if n > 1 and random.random() < 0.1: keys[-1] = keys[0]
    return "{" + ",".join("%s:%s" % (k, rval(d + 1)) for k in keys) + "}"

cases = []
hand = [b"", b" ", b"\xef\xbb\xbf{}", b"{}", b"[]", b" [ 1 , 2 ] ", b"{\"a\":1,\"a\":2}", b"{\"a\":1,\"\\u0061\":2}",
    b"[1,]", b"{\"a\":1,}", b"[01]", b"[-0]", b"[1.0]", b"[1e5]", b"NaN", b"[Infinity]", b"tru", b"true false",
    b"\"\\ud800\"", b"\"\\udc00\\ud800\"", b"\"\\ud83d\\ude00\"", b"\"\\u0000\"", b"\"a\x01b\"", b"\"\xc0\xaf\"",
    b"\"\xed\xa0\x80\"", b"\"\xf4\x90\x80\x80\"", b"\"\xe2\x82\"", b"'a'", b"[1 2]", b"{\"a\" 1}", b"{1:2}",
    b"\"\\q\"", b"\"\\u12g4\"", b"[" * 16 + b"]" * 16, b"[" * 17 + b"]" * 17, b"{\"a\":" * 16 + b"1" + b"}" * 16,
    b"{\"a\":" * 17 + b"1" + b"}" * 17, str(MAX).encode(), str(MAX + 1).encode(), str(-MAX).encode(),
    b"123abc", b"[\"\\/\"]", b"\"\t\"", b"[\n1\r\n]", b"\"\\uD83D\\uDE00\"",
    ("{" + ",".join("\"k%d\":%d" % (i, i) for i in range(256)) + "}").encode(),
    ("{" + ",".join("\"k%d\":%d" % (i, i) for i in range(257)) + "}").encode()]
for b in hand: cases.append(b)
for _ in range(3000):
    cases.append(rval().encode("utf-8", "surrogatepass"))
for _ in range(2000):
    b = bytearray(rval().encode("utf-8", "surrogatepass"))
    for _ in range(random.randrange(1, 3)):
        if not b: break
        i = random.randrange(len(b)); r = random.random()
        if r < 0.4: b[i] = random.randrange(256)
        elif r < 0.7: del b[i]
        else: b.insert(i, random.choice(b'{}[]",:\\ 0-.eu\x00\xff'))
    cases.append(bytes(b))
ok = 0
print(len(cases))
for b in cases:
    v = verdict(b)
    if v is not None: ok += 1
    print("%d %s %s" % (1 if v is not None else 0, b.hex() or "-", (v.hex() or "-") if v is not None else "-"))
print("# %d accepted of %d" % (ok, len(cases)), file=sys.stderr)
