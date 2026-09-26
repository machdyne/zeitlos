#ifndef AUTHCORE_H
#define AUTHCORE_H
/*
 * Zeitlos OS
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The password's arithmetic: PBKDF2-HMAC-SHA256, the record kept in
 * the flash key/value store, and the schedule of delays after failed
 * attempts. docs/security.md.
 *
 * No platform dependencies beyond sw/common/zsha256.c, like kvlog.c --
 * sw/os/tests/test_auth.c checks all of it on the host against the
 * published PBKDF2 test vectors.
 */
#include <stdint.h>
#include "../common/zsha256.h"

#define AUTH_SALT_LEN     16
#define AUTH_DK_LEN       32
#define AUTH_REC_LEN      (1 + 1 + 4 + AUTH_SALT_LEN + AUTH_DK_LEN)   // 54
#define AUTH_REC_VERSION  1

#define AUTH_REC_NET_OK   0x01u   // long enough for network logins (Z_AUTH_NET_MIN)

typedef struct {
	uint8_t flags;
	uint32_t iterations;
	uint8_t salt[AUTH_SALT_LEN];
	uint8_t dk[AUTH_DK_LEN];
} auth_rec_t;

// PBKDF2-HMAC-SHA256 (RFC 8018), one 32-byte block -- all a password
// check needs. `iterations` >= 1.
void auth_pbkdf2(const uint8_t *pw, uint32_t pwlen, const uint8_t *salt,
	uint32_t saltlen, uint32_t iterations, uint8_t out[AUTH_DK_LEN]);

// The stored form: version, flags, iterations (LE), salt, key.
void auth_rec_pack(const auth_rec_t *r, uint8_t out[AUTH_REC_LEN]);
int auth_rec_unpack(const uint8_t *in, uint32_t len, auth_rec_t *r);   // 0 ok, -1 not a record

// Equal, in time that does not depend on where they differ.
int auth_eq(const uint8_t *a, const uint8_t *b, uint32_t n);

// Milliseconds to refuse further attempts after `fails` consecutive
// failures: none for the first three, then 1 s doubling to 60 s.
uint32_t auth_backoff_ms(uint32_t fails);

#endif
