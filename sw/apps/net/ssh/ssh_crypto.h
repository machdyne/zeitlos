#ifndef SSH_CRYPTO_H
#define SSH_CRYPTO_H

#include <stdint.h>
#include <stdbool.h>

#include "ssh_sha256.h"

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * The transport cipher (`chacha20-poly1305@openssh.com`) and RFC 4253
 * key derivation.
 *
 * -- Why this cipher, and the consequence --
 *
 * It is the only one we can offer. sw/ext/monocypher has no AES, so
 * every `aes*-ctr` and `aes*-gcm` suite is off the table, and this is
 * what is left: ChaCha20 and Poly1305, both of which Monocypher does
 * have, in exactly the shapes OpenSSH's construction needs
 * (`crypto_chacha20_djb` takes an 8-byte nonce and an explicit 64-bit
 * block counter; Poly1305 is incremental).
 *
 * OpenSSH enables it by default, so this works against essentially
 * every server in practice. But a hardened server that has disabled it
 * will refuse us in KEXINIT and there is no fallback to negotiate --
 * ssh_kex.c reports that as a specific error rather than a generic
 * handshake failure, because "this server does not offer the one
 * cipher we have" is a thing the user can act on.
 *
 * -- How the construction works --
 *
 * Not a standard AEAD, and the differences are the part people get
 * wrong:
 *
 *   - The key is 64 bytes, not 32. The FIRST 32 are K_2 (payload), the
 *     SECOND 32 are K_1 (packet length). Two independent ChaCha20 keys.
 *   - The 4-byte packet length is encrypted separately, with K_1, at
 *     block counter 0. This exists so a receiver can learn how long a
 *     packet is before it has enough of it to authenticate.
 *   - The Poly1305 key is the first 32 bytes of the K_2 keystream at
 *     block counter 0. The payload is then encrypted with K_2 starting
 *     at block counter 1.
 *   - The nonce for both keys is the 64-bit packet sequence number, big
 *     endian. There is no separate IV -- RFC 4253's derived keys A and
 *     B are simply unused, as are integrity keys E and F, since the
 *     AEAD tag replaces the MAC.
 *   - The tag covers the ENCRYPTED length bytes plus the encrypted
 *     payload.
 *   - The 4-byte length is NOT part of the padding computation, unlike
 *     a classic SSH cipher. Padding makes (payload + padding_len_byte +
 *     padding) a multiple of 8.
 *
 * -- The rule that matters for security --
 *
 * NOTHING FROM A PACKET IS TRUSTED UNTIL THE TAG VERIFIES. The
 * decrypted length is used to decide how many bytes to read, and that
 * is unavoidable, but the payload is not parsed, dispatched or acted
 * on until ssh_aead_open() has returned true. The tag comparison is
 * `crypto_verify16`, which is constant time; a byte-by-byte memcmp
 * here would leak the tag through timing and let an attacker forge
 * packets one byte at a time.
 */

#define SSH_AEAD_KEY_LEN   64		// K_2 || K_1
#define SSH_AEAD_TAG_LEN   16
#define SSH_AEAD_LEN_LEN   4

// One direction's cipher state. Two of these per session.
typedef struct {
	uint8_t key[SSH_AEAD_KEY_LEN];
	uint64_t seq;					// packet sequence number = nonce
	bool active;					// false until NEWKEYS; plaintext before
} ssh_cipher;

void ssh_cipher_init(ssh_cipher *c);
void ssh_cipher_wipe(ssh_cipher *c);

// Installs a freshly derived key and marks the direction encrypted.
// The sequence number is deliberately NOT reset: RFC 4253 says it runs
// continuously from the first packet of the connection and never
// restarts, including across a rekey. Resetting it would reuse nonces
// with a new key, which is survivable, and reuse them with the SAME key
// after a rekey that derived the same material, which is not.
void ssh_cipher_set_key(ssh_cipher *c, const uint8_t key[SSH_AEAD_KEY_LEN]);

// Decrypts just the 4-byte length field, so the caller knows how many
// more bytes to wait for. Does NOT authenticate anything -- the value
// returned is attacker controlled and must be range checked by the
// caller before being used to size anything.
uint32_t ssh_aead_peek_len(const ssh_cipher *c, const uint8_t enc_len[4]);

// Authenticates and decrypts a whole packet in place.
//
// `pkt` is the complete on-wire packet: 4 encrypted length bytes,
// `payload_len` encrypted payload bytes, then a 16-byte tag. On
// success the payload region holds plaintext and true is returned; on
// failure nothing is written and false is returned. Advances seq
// either way -- a failed tag ends the session, so there is no case
// where a caller retries with the same sequence number.
bool ssh_aead_open(ssh_cipher *c, uint8_t *pkt, uint32_t payload_len);

// Encrypts and tags a whole packet in place, same layout. The caller
// has already written the plaintext length and payload.
void ssh_aead_seal(ssh_cipher *c, uint8_t *pkt, uint32_t payload_len);

// -- key derivation (RFC 4253 section 7.2) --
//
// K_x = HASH(K || H || X || session_id), extended if needed by
// K_x = K_x || HASH(K || H || K_x). SHA-256 gives 32 bytes and this
// cipher wants 64, so exactly one extension round runs.
//
// Only 'C' and 'D' (the two encryption keys) are ever derived here.
// 'A'/'B' are IVs this cipher does not use, and 'E'/'F' are integrity
// keys the AEAD tag replaces -- deriving them would be dead code.
//
// `k_mpint` is the shared secret ALREADY IN MPINT FORM (see
// ssh_wr_mpint), not the raw 32 bytes. That is not a convenience: the
// mpint encoding is what the server hashed, and passing the raw value
// produces keys that differ from the server's about half the time.
void ssh_derive_key(uint8_t out[SSH_AEAD_KEY_LEN],
	const uint8_t *k_mpint, uint32_t k_mpint_len,
	const uint8_t h[SSH_SHA256_DIGEST], char letter,
	const uint8_t session_id[SSH_SHA256_DIGEST]);

// Formats a host key fingerprint the way OpenSSH does:
// "SHA256:" followed by unpadded base64 of the SHA-256 of the key
// blob. Matches `ssh-keygen -lf`, so a user can compare it against
// what their other machines show. `out` needs 56 bytes.
void ssh_fingerprint(char *out, uint32_t out_cap,
	const uint8_t *keyblob, uint32_t len);

#endif
