#ifndef SHA384_H
#define SHA384_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * SHA-384, for certificate chains that use it.
 *
 * -- why it is needed --
 *
 * A large share of real ECDSA chains sign with ecdsa-with-SHA384, and
 * plenty of RSA intermediates use sha384WithRSAEncryption. A client
 * with only SHA-256 parses those chains, verifies the leaf, and then
 * cannot check the signature above it. TLS 1.3 itself never needs
 * SHA-384 here -- the handshake transcript and key schedule are
 * SHA-256 because the cipher suite says so (tls_crypto.h) -- but the
 * certificates in the chain were signed long before this connection
 * and are not constrained by it.
 *
 * -- built on Monocypher's SHA-512 --
 *
 * SHA-384 is SHA-512 with a different initial value, truncated to 48
 * bytes. Nothing else differs, so this reuses the vendored
 * compression function rather than adding a second one.
 *
 * That means reaching into crypto_sha512_ctx to replace the IV, and
 * monocypher-ed25519.h says of that struct: "they may change without
 * notice". So tests/test_sha384.c checks a published vector -- a
 * Monocypher update that changes the layout fails there rather than
 * silently producing wrong digests, which for a signature check would
 * mean rejecting every valid chain.
 */

#include <stdint.h>

#define SHA384_DIGEST 48

void sha384(uint8_t out[SHA384_DIGEST], const void *data, uint32_t len);

#endif
