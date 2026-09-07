/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * See sha384.h.
 */

#include <string.h>

#include "sha384.h"
#include "../../ext/monocypher/monocypher-ed25519.h"

// The SHA-384 initial hash value (FIPS 180-4 section 5.3.4): the
// fractional parts of the square roots of the 9th through 16th
// primes. This is the ONLY thing that differs from SHA-512 before the
// final truncation.
static const uint64_t sha384_iv[8] = {
	0xcbbb9d5dc1059ed8ULL, 0x629a292a367cd507ULL,
	0x9159015a3070dd17ULL, 0x152fecd8f70e5939ULL,
	0x67332667ffc00b31ULL, 0x8eb44a8768581511ULL,
	0xdb0c2e0d64f98fa7ULL, 0x47b5481dbefa4fa4ULL,
};

void sha384(uint8_t out[SHA384_DIGEST], const void *data, uint32_t len) {

	crypto_sha512_ctx ctx;
	uint8_t full[64];

	// init() sets up the counters and buffer state as well as the IV,
	// so it is called first and only the IV is replaced -- rather
	// than zeroing the struct here and guessing at the rest of its
	// initial state.
	crypto_sha512_init(&ctx);
	memcpy(ctx.hash, sha384_iv, sizeof(sha384_iv));

	crypto_sha512_update(&ctx, (const uint8_t *)data, len);
	crypto_sha512_final(&ctx, full);

	// Truncation is part of the definition, not an optimisation: the
	// remaining 16 bytes are discarded.
	memcpy(out, full, SHA384_DIGEST);

	crypto_wipe(full, sizeof(full));
	crypto_wipe(&ctx, sizeof(ctx));

}
