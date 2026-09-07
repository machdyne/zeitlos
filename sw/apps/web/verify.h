#ifndef VERIFY_H
#define VERIFY_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Certificate chain validation: the step that turns "encrypted to
 * somebody" into "encrypted to the host I asked for".
 *
 * -- the root store is a callback --
 *
 * This file does not open a file, so the store can be a directory on
 * the card in the app and an array in memory in the tests. Same
 * pattern page.h uses for the spool, and for the same reason: the
 * logic worth testing is the chain walk, and it should not need a
 * filesystem to exercise.
 *
 * -- what is checked, in order --
 *
 *   1. The leaf matches the hostname. FIRST, before any signature
 *      work: a certificate that is perfectly valid for another
 *      domain is the likeliest thing an attacker has, and there is
 *      no reason to spend three signature verifications discovering
 *      it.
 *   2. Every certificate is within its validity dates.
 *   3. Each certificate is issued by the next, each issuer is a
 *      usable CA, and each signature verifies.
 *   4. The last certificate is signed by something in the local
 *      store.
 *
 * Step 4 is the one that carries the whole weight. The server's own
 * last certificate is NOT trusted for being last -- a self-signed
 * root supplied by the server is the simplest possible forged chain.
 *
 * -- what is deliberately not checked --
 *
 *   - **No revocation.** No CRLs, no OCSP, no stapling. An OCSP
 *     fetch during a handshake on a machine with one TCP connection
 *     is not something to attempt lightly, and a client that fails
 *     open on revocation -- which is what every browser does -- gains
 *     very little for the complexity. Stapled OCSP is the version
 *     worth having later, since it costs no extra connection.
 *   - **No name constraints and no policy constraints.** Both matter
 *     for enterprise PKI and neither appears on the public web.
 *   - **No chain reordering or path building.** If a server sends a
 *     chain out of order, or includes an unrelated certificate, this
 *     says so rather than searching for a valid path through the
 *     set. Real servers overwhelmingly send an ordered chain.
 *   - **No SHA-384 or SHA-512 signatures.** Recognised and reported
 *     as unsupported, because "this chain uses SHA-384" is
 *     actionable where "signature invalid" is not.
 */

#include <stdint.h>
#include <stdbool.h>

#include "x509.h"

// Certificates accepted from the server. Real chains are two or
// three; five leaves room for a cross-signed intermediate without
// letting a peer make this walk unbounded.
#define VERIFY_MAX_CHAIN 5

typedef struct {
	const uint8_t	*der;
	uint32_t		len;
} cert_blob_t;

// Looks up a trusted root whose SUBJECT equals `issuer`, parses it
// into `out`, and returns true if one was found.
//
// The comparison is on the encoded DN, byte for byte -- see x509.h.
typedef bool (*verify_root_fn)(void *user, const der_t *issuer,
	x509_cert_t *out);

// `chain[0]` is the leaf, as TLS sends it. `now` is Unix seconds; 0
// means the clock is not set, which is treated as a failure rather
// than as a licence to skip the date check.
//
// On failure `err` receives a short reason suitable for showing to a
// person. It is never NULL when this returns false.
// Milliseconds since some fixed point. Optional: set it and
// verify_chain() reports what each signature check cost.
//
// A function pointer rather than a direct call to z_uptime_ticks()
// because this file is host-tested, and pulling in a Zeitlos header
// for a diagnostic would end that.
typedef uint32_t (*verify_clock_fn)(void);

void verify_set_clock(verify_clock_fn clock);

// Forgets any cached verification result. Call if the root store
// changes; not needed otherwise.
void verify_flush_cache(void);

bool verify_chain(const cert_blob_t *chain, uint32_t n,
	const char *host, int64_t now,
	verify_root_fn find_root, void *user,
	const char **err);

#endif
