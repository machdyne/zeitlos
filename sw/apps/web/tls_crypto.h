#ifndef TLS_CRYPTO_H
#define TLS_CRYPTO_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The cryptographic half of TLS 1.3: HMAC, HKDF, the key schedule,
 * and the AEAD. No sockets, no state machine, no parsing -- every
 * function here is a pure transformation of bytes, which is what
 * lets tests/test_tls_crypto.c check all of it against vectors on
 * the build machine.
 *
 * That separation is not stylistic. A key schedule bug produces a
 * handshake that fails with "decrypt error" from a server that will
 * not tell you why, at which point you are debugging four layers at
 * once with no visibility into any of them. Getting these functions
 * right FIRST, against known answers, removes them from the list of
 * suspects.
 *
 * -- one cipher suite --
 *
 * TLS_CHACHA20_POLY1305_SHA256, and nothing else. Not a limitation
 * accepted reluctantly: it is the only suite whose primitives this
 * tree already has, and every one of them is already used elsewhere.
 *
 *   X25519       sw/ext/monocypher   crypto_x25519()
 *   ChaCha20     sw/ext/monocypher   crypto_aead_*_ietf()
 *   Poly1305     sw/ext/monocypher   (same, RFC 8439 construction)
 *   SHA-256      sw/apps/net/ssh     ssh_sha256_*()
 *
 * The AES-GCM suites would need an AES implementation and a GHASH,
 * neither of which exists here and both of which are constant-time
 * minefields on a CPU with no cache and no AES instructions. Every
 * TLS 1.3 server must implement TLS_AES_128_GCM_SHA256, and in
 * practice essentially all of them also implement this one -- it is
 * what every mobile client without AES hardware negotiates.
 *
 * -- where SHA-256 comes from --
 *
 * sw/common/zsha256.c. It used to live in sw/apps/net/ssh/ and moved
 * when this became its second consumer; ssh's header is now a
 * forwarding shim so no SSH source changed. See sw/common/zsha256.h,
 * which is also the seam a hardware implementation would replace.
 */

#include <stdint.h>
#include <stdbool.h>

#define TLS_HASH_LEN   32		// SHA-256
#define TLS_KEY_LEN    32		// ChaCha20
#define TLS_IV_LEN     12
#define TLS_TAG_LEN    16

// Longest output HKDF-Expand is ever asked for here. The schedule
// never needs more than a hash length or a key length; the bound
// exists so tls_hkdf_expand() can refuse rather than loop.
#define TLS_HKDF_MAX   64

// -- HMAC-SHA256 (RFC 2104) ----------------------------------------

void tls_hmac(uint8_t out[TLS_HASH_LEN],
	const uint8_t *key, uint32_t key_len,
	const uint8_t *data, uint32_t data_len);

// -- HKDF (RFC 5869) -----------------------------------------------

void tls_hkdf_extract(uint8_t prk[TLS_HASH_LEN],
	const uint8_t *salt, uint32_t salt_len,
	const uint8_t *ikm, uint32_t ikm_len);

// Returns false if out_len exceeds TLS_HKDF_MAX.
bool tls_hkdf_expand(uint8_t *out, uint32_t out_len,
	const uint8_t prk[TLS_HASH_LEN],
	const uint8_t *info, uint32_t info_len);

// -- TLS 1.3 label expansion (RFC 8446 section 7.1) ----------------
//
//   struct {
//     uint16 length;
//     opaque label<7..255>   = "tls13 " + Label;
//     opaque context<0..255>;
//   } HkdfLabel;
//
// `label` is the bare label ("key", "iv", "finished", ...); the
// "tls13 " prefix is added here. Getting that prefix wrong, or the
// length prefixes, produces keys that are wrong in a way no error
// message will ever name.
bool tls_expand_label(uint8_t *out, uint32_t out_len,
	const uint8_t secret[TLS_HASH_LEN], const char *label,
	const uint8_t *context, uint32_t context_len);

// Derive-Secret(Secret, Label, Messages), where the caller has
// already hashed Messages.
void tls_derive_secret(uint8_t out[TLS_HASH_LEN],
	const uint8_t secret[TLS_HASH_LEN], const char *label,
	const uint8_t transcript[TLS_HASH_LEN]);

// -- the key schedule (RFC 8446 section 7.1) -----------------------
//
// Three secrets, each derived from the last:
//
//   early      = Extract(salt = 0,             IKM = PSK or zeros)
//   handshake  = Extract(salt = Derived(early), IKM = ECDHE shared)
//   master     = Extract(salt = Derived(hs),    IKM = zeros)
//
// The Derived() step between them is the one that is easy to leave
// out, because nothing about the diagram in the RFC makes it look
// load bearing.

// No PSK: the IKM is 32 zero bytes.
void tls_early_secret(uint8_t out[TLS_HASH_LEN]);

void tls_handshake_secret(uint8_t out[TLS_HASH_LEN],
	const uint8_t early[TLS_HASH_LEN],
	const uint8_t ecdhe[TLS_KEY_LEN]);

void tls_master_secret(uint8_t out[TLS_HASH_LEN],
	const uint8_t handshake[TLS_HASH_LEN]);

// A traffic secret into the key and IV that actually protect records.
void tls_traffic_keys(uint8_t key[TLS_KEY_LEN], uint8_t iv[TLS_IV_LEN],
	const uint8_t secret[TLS_HASH_LEN]);

// The Finished message's verify_data: HMAC(finished_key, transcript),
// where finished_key = Expand-Label(base, "finished", "", 32).
void tls_finished(uint8_t out[TLS_HASH_LEN],
	const uint8_t base_secret[TLS_HASH_LEN],
	const uint8_t transcript[TLS_HASH_LEN]);

// Post-handshake rekeying (RFC 8446 section 7.2). Rewrites `secret`
// in place with its successor.
void tls_update_traffic_secret(uint8_t secret[TLS_HASH_LEN]);

// -- record protection ---------------------------------------------

// The per-record nonce: the static IV, XORed with the 64-bit sequence
// number right-aligned and big-endian (RFC 8446 section 5.3).
//
// Right-aligned and BIG-endian. Both halves of that get written the
// other way round by people who have just come from a little-endian
// wire format, and the result is a nonce that is correct for record
// zero and wrong for every record after it -- so the handshake
// completes and the first application record fails.
void tls_nonce(uint8_t out[TLS_IV_LEN], const uint8_t iv[TLS_IV_LEN],
	uint64_t seq);

// AEAD_CHACHA20_POLY1305 (RFC 8439). In-place is allowed: `out` may
// equal the input pointer.
void tls_aead_seal(uint8_t *out, uint8_t tag[TLS_TAG_LEN],
	const uint8_t key[TLS_KEY_LEN], const uint8_t nonce[TLS_IV_LEN],
	const uint8_t *ad, uint32_t ad_len,
	const uint8_t *plain, uint32_t len);

// Returns false on a tag mismatch, in which case `out` is left
// wiped rather than holding whatever the forged ciphertext decrypted
// to. A caller that treats the return value as advisory is the whole
// reason authenticated encryption stops working.
bool tls_aead_open(uint8_t *out,
	const uint8_t key[TLS_KEY_LEN], const uint8_t nonce[TLS_IV_LEN],
	const uint8_t *ad, uint32_t ad_len,
	const uint8_t *cipher, uint32_t len, const uint8_t tag[TLS_TAG_LEN]);

#endif
