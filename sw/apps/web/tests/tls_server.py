#!/usr/bin/env python3
#
# Zeitlos
# Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
#
# A throwaway TLS 1.3 server for tests/test_tls.c to handshake
# against. Generates a self-signed certificate on the fly, serves one
# fixed HTTP response, exits.
#
#   python3 tls_server.py <port> [--suite CHACHA20|AES]
#
# WHY A REAL SERVER RATHER THAN A RECORDED TRACE
#
# A recorded handshake cannot be replayed against a client: the
# client's key share is fresh every time, so every secret differs and
# nothing after the ServerHello decrypts. Testing a TLS client
# therefore means talking to something that can actually do the key
# exchange.
#
# OpenSSL, through Python's ssl module, is that something -- and it is
# strict in the ways that matter. It rejects a malformed ClientHello,
# a wrong transcript, a bad Finished and a mis-framed record, and it
# does so on the build machine in milliseconds rather than on an FPGA
# over a UART against a remote server that answers every mistake with
# the same uninformative alert.

import socket, ssl, subprocess, sys, tempfile, os, threading

# The body size is a parameter, and the default is LARGE.
#
# The first version served 11 bytes, which fits in one TLS record and
# therefore never exercised record framing at all. On hardware the
# record layer failed part way through a real page -- deterministically,
# at the same offset twice -- and no test here could have caught it,
# because no test here ever sent a second record.
#
# 200KB is bigger than a TLS record (16KB max), bigger than the app's
# spool buffer, and comparable to a real page.
BODY_SIZE = int(sys.argv[sys.argv.index("--body") + 1]) \
    if "--body" in sys.argv else 200000

# Not constant bytes: a repeating pattern makes a dropped or
# duplicated span visible as a position, rather than as a length that
# happens to be wrong.
BODY = bytes(((i * 7) ^ (i >> 8)) & 0xFF for i in range(BODY_SIZE))
RESPONSE = (b"HTTP/1.1 200 OK\r\n"
            b"Content-Type: text/plain\r\n"
            b"Content-Length: %d\r\n"
            b"Connection: close\r\n\r\n" % len(BODY)) + BODY

def make_cert(d, der_out):
    key = os.path.join(d, "k.pem"); crt = os.path.join(d, "c.pem")
    subprocess.run(
        ["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
         "-keyout", key, "-out", crt, "-days", "1",
         "-subj", "/CN=localhost",
         "-addext", "subjectAltName=DNS:localhost",
         "-addext", "basicConstraints=critical,CA:TRUE"],
        check=True, capture_output=True)
    # The DER form is written out so the client under test can use it
    # as its ONE trusted root. Without that the handshake is supposed
    # to fail, and the negative run in test_tls.c checks exactly that.
    #
    # A SAN is required: the client has no CN fallback, deliberately
    # (see x509.h), so a certificate with only a Common Name matches
    # nothing.
    subprocess.run(
        ["openssl", "x509", "-in", crt, "-outform", "DER",
         "-out", der_out],
        check=True, capture_output=True)
    return crt, key

def main():
    port = int(sys.argv[1])
    suite = "CHACHA20" if "--suite" not in sys.argv else \
        sys.argv[sys.argv.index("--suite") + 1]

    der_out = sys.argv[sys.argv.index("--cert-der") + 1] \
        if "--cert-der" in sys.argv else "/tmp/tls_server_cert.der"

    d = tempfile.mkdtemp()
    crt, key = make_cert(d, der_out)

    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_3
    ctx.maximum_version = ssl.TLSVersion.TLSv1_3
    ctx.load_cert_chain(crt, key)

    # Pin the suite so the test exercises the one this client
    # implements, rather than whichever OpenSSL happens to prefer.
    if suite == "CHACHA20":
        try:
            ctx.set_ciphers("TLS_CHACHA20_POLY1305_SHA256")
        except ssl.SSLError:
            pass

    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(1)
    sys.stderr.write("tls_server: listening on %d\n" % port)
    sys.stderr.flush()

    srv.settimeout(30)
    try:
        conn, _ = srv.accept()
    except socket.timeout:
        sys.stderr.write("tls_server: nothing connected\n")
        return 1

    try:
        tls = ctx.wrap_socket(conn, server_side=True)
    except Exception as e:
        sys.stderr.write("tls_server: handshake failed: %s\n" % e)
        return 1

    sys.stderr.write("tls_server: handshake ok, %s / %s\n"
                     % (tls.version(), tls.cipher()[0]))

    try:
        req = tls.recv(4096)
        sys.stderr.write("tls_server: got %d bytes of request, "
                         "sending %d\n" % (len(req), len(BODY)))
        tls.sendall(RESPONSE)
        tls.unwrap()
    except Exception as e:
        sys.stderr.write("tls_server: %s\n" % e)
    finally:
        conn.close()

    return 0

if __name__ == "__main__":
    sys.exit(main())
