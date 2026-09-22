#!/usr/bin/env python3
# Zeitlos -- builds the corpus for test_inflate.c.
#
#   python3 gen_inflate_corpus.py /tmp/gzc
#
# Compressed by Python's gzip/zlib, i.e. by real zlib, so the decoder
# is checked against an encoder it had no hand in. Not checked in: it
# is 400KB of data that any machine can regenerate in a second.
import gzip, zlib, random, sys, os
out = sys.argv[1] if len(sys.argv) > 1 else "."
os.makedirs(out, exist_ok=True)
def w(name, data): open(os.path.join(out, name), "wb").write(data)
random.seed(7)

# Ordinary text and markup: dynamic Huffman blocks, the common case.
text = (b'the quick brown fox jumps over the lazy dog. ' * 400 +
        b'<html><body><p>Zeitlos</p></body></html>' * 200)
w('text.raw', text); w('text.gz', gzip.compress(text, 9))
w('text.zz', zlib.compress(text, 9))

# Incompressible: STORED blocks, which take a different path and are
# the only place LEN/~LEN is checked.
rnd = bytes(random.randrange(256) for _ in range(20000))
w('rand.raw', rnd); w('rand.gz', gzip.compress(rnd, 9))

# Highly repetitive: long matches, maximum distances, and the
# self-overlapping copies that encode a run.
rep = b'A'*50000 + b'AB'*20000 + b'\0'*30000
w('rep.raw', rep); w('rep.gz', gzip.compress(rep, 9))

# Larger than the 32KB window, so back-references reach into history
# the caller has long since taken away.
big = bytes((i*7 ^ (i >> 5)) & 0xff for i in range(300000))
w('big.raw', big); w('big.gz', gzip.compress(big, 6))
print("corpus in", out)

# gzip's optional header fields (zinflate.h, gzip_fields): what `gzip
# file` writes -- a file name -- and a header with all four, built by
# hand to RFC 1952, its header CRC correct although zinflate skips it.
import struct
fn = b'the quick brown fox jumps over the lazy dog, again. ' * 300
w('fields.raw', fn)
with gzip.GzipFile(filename='notes.txt', mode='wb', fileobj=open(os.path.join(out, 'fname.gz'), 'wb'), mtime=0) as g:
    g.write(fn)
co = zlib.compressobj(9, zlib.DEFLATED, -15)
body = co.compress(fn) + co.flush()
extra = b'Zt' + struct.pack('<H', 4) + b'ZEIT'
hdr = bytes([0x1f, 0x8b, 8, 0x04 | 0x08 | 0x10 | 0x02]) + struct.pack('<I', 0) + bytes([2, 3])
hdr += struct.pack('<H', len(extra)) + extra + b'notes.txt\0' + b'a comment, as gzip -c might carry\0'
hdr += struct.pack('<H', zlib.crc32(hdr) & 0xffff)
w('fields.gz', hdr + body + struct.pack('<II', zlib.crc32(fn), len(fn)))

