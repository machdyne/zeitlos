#!/usr/bin/env python3
#
# Zeitlos
# Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
#
# The two certificates gen_certs.sh cannot make with OpenSSL 3.0's
# command line. Called from there; not usually run directly.

import sys, datetime, os
from cryptography import x509
from cryptography.x509.oid import NameOID
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa

d = sys.argv[1] if len(sys.argv) > 1 else "."

ca_key = serialization.load_pem_private_key(
    open(os.path.join(d, "ca.key"), "rb").read(), password=None)
ca = x509.load_pem_x509_certificate(open(os.path.join(d, "ca.pem"), "rb").read())

def issue(name, cn, sans, nb, na, path):
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    b = (x509.CertificateBuilder()
         .subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, cn)]))
         .issuer_name(ca.subject)
         .public_key(key.public_key())
         .serial_number(x509.random_serial_number())
         .not_valid_before(nb)
         .not_valid_after(na)
         .add_extension(x509.BasicConstraints(ca=False, path_length=None), False))
    if sans:
        b = b.add_extension(
            x509.SubjectAlternativeName([x509.DNSName(s) for s in sans]), False)
    cert = b.sign(ca_key, hashes.SHA256())
    open(os.path.join(d, path), "wb").write(
        cert.public_bytes(serialization.Encoding.DER))

issue("expired", "old.example.com", ["old.example.com"],
      datetime.datetime(2020, 1, 1), datetime.datetime(2020, 1, 2),
      "expired.der")

# No SAN extension at all.
issue("cnonly", "cn.example.com", None,
      datetime.datetime(2024, 1, 1), datetime.datetime(2034, 1, 1),
      "cnonly.der")

print("gen_certs_extra: wrote expired.der cnonly.der")
