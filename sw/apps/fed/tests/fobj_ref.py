#!/usr/bin/env python3
# An independent implementation of zfed's object rules (docs/fed.md,
# "Objects" and "Encoding, exactly"), written from the spec -- not from
# fobj.c. Used by gen_fobj_vectors.py and check_fobj.py.
import hashlib, re
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey, Ed25519PrivateKey
from cryptography.hazmat.primitives import serialization
from cryptography.exceptions import InvalidSignature

MAX = 2**53
NAME = re.compile(rb"^[a-z0-9._-]+(/[a-z0-9._-]+)*$")
TYPE = re.compile(rb"^[a-z0-9._-]+$")
DEC = re.compile(rb"^(0|[1-9][0-9]*)$")
HEX64 = re.compile(rb"^[0-9a-f]{64}$")
SIGLINE = re.compile(rb"^\nsig: [0-9a-f]{128}\n$")
PREFIX = b"zfed object\x00"

def dec(v):
    if not DEC.match(v): return None
    x = int(v)
    return x if x <= MAX else None

def parse(b, now=0):
    """(id, size, origin, sig) if b begins with an object that keeps every
    rule but the signature; None if not."""
    if not b.startswith(b"ZFED1\n"): return None
    pos, vals = 6, {}
    for name in ("topic", "type", "format", "kind", "origin", "time", "seq", "key", "len"):
        if name == "key" and vals.get("kind") != b"state": continue
        nl = b.find(b"\n", pos)
        if nl < 0 or nl >= 1024: return None
        line, prefix = b[pos:nl], name.encode() + b": "
        if not line.startswith(prefix): return None
        v = line[len(prefix):]
        if not v or any(c < 0x21 or c > 0x7e for c in v): return None
        vals[name] = v
        pos = nl + 1
    if pos >= len(b) or b[pos] != 0x0a: return None
    pos += 1
    if pos > 1024: return None
    if not NAME.match(vals["topic"]) or len(vals["topic"]) > 96: return None
    if not TYPE.match(vals["type"]) or len(vals["type"]) > 64: return None
    if vals["format"] not in (b"json", b"text", b"bytes"): return None
    if vals["kind"] not in (b"log", b"state"): return None
    if not HEX64.match(vals["origin"]): return None
    t, s, n = dec(vals["time"]), dec(vals["seq"]), dec(vals["len"])
    if t is None or s is None or n is None or s == 0 or n > 16384: return None
    if now and t > now + 86400: return None
    if "key" in vals and (not NAME.match(vals["key"]) or len(vals["key"]) > 64): return None
    if pos + n + 135 > len(b): return None
    payload = b[pos:pos + n]
    if not SIGLINE.match(b[pos + n:pos + n + 135]): return None
    if vals["format"] in (b"json", b"text"):
        if b"\x00" in payload: return None
        try: payload.decode("utf-8")
        except UnicodeError: return None
    oid = hashlib.sha256(b[:pos + n]).digest()
    return oid, pos + n + 135, bytes.fromhex(vals["origin"].decode()), bytes.fromhex(b[pos + n + 6:pos + n + 134].decode())

def verify(oid, origin, sig):
    try:
        Ed25519PublicKey.from_public_bytes(origin).verify(sig, PREFIX + oid)
        return True
    except (InvalidSignature, ValueError):
        return False

def make(sk, topic, typ, fmt, kind, time, seq, payload, key=None):
    pub = sk.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    h = b"ZFED1\ntopic: %s\ntype: %s\nformat: %s\nkind: %s\norigin: %s\ntime: %d\nseq: %d\n" % (
        topic, typ, fmt, kind, pub.hex().encode(), time, seq)
    if kind == b"state": h += b"key: %s\n" % key
    h += b"len: %d\n\n" % len(payload)
    body = h + payload
    oid = hashlib.sha256(body).digest()
    return body + b"\nsig: " + sk.sign(PREFIX + oid).hex().encode() + b"\n"
