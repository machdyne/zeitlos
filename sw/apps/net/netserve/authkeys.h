#ifndef AUTHKEYS_H
#define AUTHKEYS_H
/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The SSH keys allowed to log in: /user/authkeys, OpenSSH's
 * authorized_keys format. docs/netserve.md, "SSH keys".
 *
 *     ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAA... phil@laptop
 *     ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAABAQ... phil@desktop
 *     SHA256:4ADgAqIUtDClAPaRQO8ax85ZPlUk7o/vfGoisvYx7JQ
 *     # a comment
 *
 * A key line (as ssh-keygen writes a .pub file), or a fingerprint -- as
 * `ssh-keygen -lf` prints it, or as 64 hex digits -- which allows that
 * key whatever its type. Every entry is kept as the SHA-256 of the key
 * blob: the client sends the whole key when it logs in, and a key
 * matching the hash IS the listed key (a second preimage of SHA-256 is
 * not a practical attack). That is also why a fingerprint line works.
 *
 * Refused, and reported: a line with OpenSSH options before the key
 * (from=, command=, no-pty ...) -- ignoring a restriction would grant
 * more than the file says -- an RSA key under 2048 bits or over 4096,
 * and anything malformed.
 *
 * No platform in here beyond sw/common/zsha256, monocypher and
 * sw/apps/web/rsa.c: tests/test_authkeys.c runs it on the host.
 */
#include <stdint.h>
#include <stdbool.h>

#define AK_MAX         32
#define AK_ANY         0        // a fingerprint line: any key type
#define AK_ED25519     1
#define AK_RSA         2

typedef struct {
	uint8_t fp[32];
	uint8_t type;
} ak_entry_t;

typedef struct {
	ak_entry_t e[AK_MAX];
	int n;
	int refused;                // lines refused (see warn)
} authkeys_t;

// `warn` (may be NULL) hears about each refused line.
void authkeys_parse(authkeys_t *ak, const char *text, uint32_t len,
	void (*warn)(int line, const char *why));

// The type of a key blob (AK_ED25519, AK_RSA), or -1 if it is neither
// or malformed; *bits gets an RSA key's modulus size.
int authkeys_blob_type(const uint8_t *blob, uint32_t len, uint32_t *bits);

// May `blob`, signing with `alg` ("ssh-ed25519", "rsa-sha2-256"), log
// in? Listed, of the type the algorithm needs, and (RSA) 2048-4096 bits.
bool authkeys_allows(const authkeys_t *ak, const char *alg, const uint8_t *blob, uint32_t len);

// Is `sig` (string alg || string signature) a valid signature by `blob`
// over `data`?
bool authkeys_verify(const char *alg, const uint8_t *blob, uint32_t blob_len,
	const uint8_t *sig, uint32_t sig_len, const uint8_t *data, uint32_t data_len);

// "SHA256:..." for a key blob, as ssh-keygen -lf prints it (56 bytes).
void authkeys_fingerprint(char *out, uint32_t cap, const uint8_t *blob, uint32_t len);

#endif
