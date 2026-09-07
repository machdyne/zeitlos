#!/usr/bin/env python3
"""
Zeitlos -- builds the trust store that ships on the card.

    python3 tools/mkroots.py sw/apps/web/roots/cacert.pem -o roots.der

-- what this is --

A concatenation of DER certificates with an INDEX in front: sw/apps/web
reads the index once and then seeks straight to the root it needs,
instead of parsing every certificate in the file to find one.

That is not a micro-optimisation. Measured on hardware, scanning 147
certificates off a bit-banged SD card took 1.65 SECONDS on the first
HTTPS fetch of every run. The index makes it a few kilobytes and one
seek.

-- where the certificates come from --

You cannot generate CA roots; they are other organisations'
certificates, and all a distribution does is choose which to trust.
Everything traces back to Mozilla's CA Certificate Program (the NSS
store).

The recommended input is curl's distribution of it:

    https://curl.se/ca/cacert.pem

which is Mozilla-derived, dated, and published alongside a SHA-256.
Ubuntu's /etc/ssl/certs is the same list at whatever vintage that
machine happens to have, so it works for a local build and is the
wrong thing to ship.

-- why the file is vendored and not fetched --

THE TRUST STORE IS THE TRUST ANCHOR. A build that downloads it can
silently change what every Zeitlos machine believes, and a
compromised download is a compromised device. So the PEM lives in the
tree, its hash is recorded next to it, and `--expect-sha256` makes a
changed input a build failure rather than a surprise.

Updating it is then a visible commit that a person reviews, which is
the only review this file will ever get.

-- format --

All little-endian, matching the CPU.

    magic     8 bytes   "ZROOTS\\0\\0"
    version   u32       1
    count     u32       number of certificates
    reserved  u32 x2    zero
    entries   count x 16 bytes:
                dn_hash  8 bytes   first 8 of SHA-256(subject DN, DER)
                offset   u32       from the start of the file
                length   u32
    blobs     the DER certificates, in index order

Entries are sorted by dn_hash so a lookup can stop early. The hash is
a FILTER, not an identifier: sw/apps/web still compares the full
subject DN byte for byte after parsing, so a collision costs one
wasted read rather than the wrong trust anchor.

A file that does not start with the magic is read as a plain
concatenation of DER, which is what earlier builds produced.
"""

import argparse, base64, hashlib, re, struct, sys, os

MAGIC = b"ZROOTS\0\0"

def pem_certs(text):
    """Every CERTIFICATE block in a PEM file, as DER."""
    out = []
    for m in re.finditer(
            r"-----BEGIN CERTIFICATE-----(.*?)-----END CERTIFICATE-----",
            text, re.S):
        try:
            out.append(base64.b64decode("".join(m.group(1).split())))
        except Exception:
            pass
    return out

# -- just enough DER to find the subject ---------------------------
#
# The subject is the sixth element of tbsCertificate, or the fifth
# when the optional [0] version tag is absent. Nothing else about the
# certificate is parsed here: this tool decides WHICH bytes to index,
# not whether a certificate is any good. sw/apps/web/x509.c does that,
# on the device, where it matters.

def der_read(buf, pos):
    tag = buf[pos]; pos += 1
    n = buf[pos]; pos += 1
    if n & 0x80:
        k = n & 0x7F
        n = int.from_bytes(buf[pos:pos+k], "big")
        pos += k
    return tag, pos, n

def subject_der(cert):
    tag, p, n = der_read(cert, 0)              # Certificate
    tag, p, n = der_read(cert, p)              # tbsCertificate
    end = p + n
    if cert[p] == 0xA0:                        # [0] version
        tag, q, ln = der_read(cert, p)
        p = q + ln
    for _ in range(4):                         # serial, sigalg, issuer, validity
        tag, q, ln = der_read(cert, p)
        p = q + ln
    start = p                                  # subject
    tag, q, ln = der_read(cert, p)
    return cert[start:q + ln]

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("inputs", nargs="+",
                    help="PEM file(s), or a directory of them")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--expect-sha256",
                    help="fail unless the concatenated inputs hash to this")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    raw = b""
    for path in args.inputs:
        if os.path.isdir(path):
            for name in sorted(os.listdir(path)):
                if name.endswith((".pem", ".crt")):
                    raw += open(os.path.join(path, name), "rb").read()
        else:
            raw += open(path, "rb").read()

    digest = hashlib.sha256(raw).hexdigest()
    if args.expect_sha256 and digest != args.expect_sha256:
        sys.stderr.write(
            "mkroots: input hash mismatch\n"
            "  expected %s\n  got      %s\n"
            "The trust store changed. That is a decision to review, not a\n"
            "build to rerun -- see tools/mkroots.py.\n"
            % (args.expect_sha256, digest))
        return 1

    certs = pem_certs(raw.decode("utf-8", "replace"))
    if not certs:
        sys.stderr.write("mkroots: no certificates found\n")
        return 1

    # Deduplicate: distributions ship the same root under several
    # names, and a duplicate is a wasted index entry and a wasted seek.
    seen, uniq = set(), []
    for c in certs:
        h = hashlib.sha256(c).digest()
        if h not in seen:
            seen.add(h)
            uniq.append(c)

    entries = []
    for c in uniq:
        try:
            dn = subject_der(c)
        except Exception:
            continue                            # not a certificate we can index
        entries.append((hashlib.sha256(dn).digest()[:8], c))

    entries.sort(key=lambda e: e[0])

    header = len(MAGIC) + 4 * 4
    table = 16 * len(entries)
    off = header + table

    out = bytearray()
    out += MAGIC
    out += struct.pack("<IIII", 1, len(entries), 0, 0)
    blobs = bytearray()
    for dn_hash, c in entries:
        out += dn_hash + struct.pack("<II", off + len(blobs), len(c))
        blobs += c
    out += blobs

    open(args.output, "wb").write(bytes(out))

    if not args.quiet:
        sys.stderr.write(
            "mkroots: %d certificates (%d duplicates dropped), "
            "%d bytes, index %d bytes\n"
            "mkroots: input sha256 %s\n"
            % (len(entries), len(certs) - len(uniq), len(out), table, digest))

    return 0

if __name__ == "__main__":
    sys.exit(main())
