#!/bin/sh
#
# Zeitlos
# Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
#
# Builds the certificate corpus tests/test_x509.c runs against.
#
#   sh gen_certs.sh <outdir>
#
# Real certificates from a real toolchain, not hand-written DER. The
# point is to be tested against what OpenSSL actually emits --
# including the encodings nobody would think to write by hand: the
# implicit v1 version field, the leading zero on a modulus whose top
# bit is set, GeneralizedTime on a far-future notAfter, keyUsage with
# a non-zero unused-bit count.
#
# Regenerate whenever the corpus needs extending. The .der files are
# checked in so `make test` does not need openssl.

set -e
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
OUT="${1:-certs}"
mkdir -p "$OUT"
cd "$OUT"

gen_ca() {
	openssl req -x509 -newkey rsa:2048 -nodes -keyout ca.key -out ca.pem \
		-days 3650 -subj "/CN=Zeitlos Test CA" \
		-addext "basicConstraints=critical,CA:TRUE,pathlen:1" \
		-addext "keyUsage=critical,keyCertSign,cRLSign" 2>/dev/null
	openssl x509 -in ca.pem -outform DER -out ca.der
}

# A leaf signed by the CA, with the SANs given.
leaf() {
	name="$1"; sans="$2"; days="${3:-365}"; key="${4:-rsa:2048}"
	openssl req -newkey "$key" -nodes -keyout "$name.key" -out "$name.csr" \
		-subj "/CN=$name" 2>/dev/null
	printf 'subjectAltName=%s\nbasicConstraints=CA:FALSE\n' "$sans" > "$name.ext"
	openssl x509 -req -in "$name.csr" -CA ca.pem -CAkey ca.key \
		-CAcreateserial -out "$name.pem" -days "$days" \
		-extfile "$name.ext" -sha256 2>/dev/null
	openssl x509 -in "$name.pem" -outform DER -out "$name.der"
}

gen_ca

leaf plain     "DNS:example.com,DNS:www.example.com"
leaf wildcard  "DNS:*.example.com"
# P-256, generated separately: `openssl req -newkey ec:...` wants a
# parameters FILE, not a curve name, and pointing it at stdin is how
# this script hung the first time it was run.
openssl ecparam -name prime256v1 -genkey -noout -out ecdsa.key 2>/dev/null
openssl req -new -key ecdsa.key -out ecdsa.csr -subj "/CN=ec.example.com" 2>/dev/null
printf 'subjectAltName=DNS:ec.example.com\nbasicConstraints=CA:FALSE\n' > ecdsa.ext
openssl x509 -req -in ecdsa.csr -CA ca.pem -CAkey ca.key -CAcreateserial \
	-out ecdsa.pem -days 365 -extfile ecdsa.ext -sha256 2>/dev/null
openssl x509 -in ecdsa.pem -outform DER -out ecdsa.der

# Two certificates that OpenSSL's command line cannot produce
# portably, built with python-cryptography instead:
#
#   expired.der  notBefore and notAfter both in the past. `openssl
#                x509 -req` only grew -not_before/-not_after in 3.2,
#                and this has to work on 3.0.
#   cnonly.der   a Common Name and NO subjectAltName. Must match
#                nothing at all -- see x509.h on why the CN fallback
#                is not implemented.
python3 "$SCRIPT_DIR/gen_certs_extra.py" . || {
	echo "gen_certs.sh: python-cryptography missing, skipping 2 certs" >&2
}

rm -f *.csr *.ext *.srl
echo "wrote: $(ls *.der | tr '\n' ' ')"
