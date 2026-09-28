/*
 * cryptobench on the host: the -O2 names mapped to the one build, and
 * the file calls to /tmp. Timings mean nothing here; the known answers
 * and the montmul arithmetic (against a model of the block) do.
 */
#include <stdio.h>
#include <string.h>
#include "zsha256.h"
#include "monocypher.h"
#include "monocypher-ed25519.h"

void o2_z_sha256_init(z_sha256_ctx *c) { z_sha256_init(c); }
void hw_z_sha256_init(z_sha256_ctx *c) { z_sha256_init(c); }
void hw_z_sha256_update(z_sha256_ctx *c, const void *d, uint32_t n) { z_sha256_update(c, d, n); }
void hw_z_sha256_final(z_sha256_ctx *c, uint8_t o[Z_SHA256_DIGEST]) { z_sha256_final(c, o); }
void o2_z_sha256_update(z_sha256_ctx *c, const void *d, uint32_t n) { z_sha256_update(c, d, n); }
void o2_z_sha256_final(z_sha256_ctx *c, uint8_t o[Z_SHA256_DIGEST]) { z_sha256_final(c, o); }
void o2_crypto_sha512(uint8_t h[64], const uint8_t *m, size_t n) { crypto_sha512(h, m, n); }
void o2_crypto_blake2b(uint8_t *h, size_t hs, const uint8_t *m, size_t n) { crypto_blake2b(h, hs, m, n); }
void o2_crypto_ed25519_key_pair(uint8_t sk[64], uint8_t pk[32], uint8_t seed[32]) { crypto_ed25519_key_pair(sk, pk, seed); }
void o2_crypto_ed25519_sign(uint8_t s[64], const uint8_t sk[64], const uint8_t *m, size_t n) { crypto_ed25519_sign(s, sk, m, n); }
int o2_crypto_ed25519_check(const uint8_t s[64], const uint8_t pk[32], const uint8_t *m, size_t n) { return crypto_ed25519_check(s, pk, m, n); }
void o2_crypto_x25519(uint8_t sh[32], const uint8_t sk[32], const uint8_t pk[32]) { crypto_x25519(sh, sk, pk); }
void o2_crypto_aead_lock(uint8_t *ct, uint8_t mac[16], const uint8_t key[32], const uint8_t nonce[24],
	const uint8_t *ad, size_t ads, const uint8_t *pt, size_t pts) { crypto_aead_lock(ct, mac, key, nonce, ad, ads, pt, pts); }

static FILE *f;
int fs_open_write(const char *name) { (void)name; f = fopen("/tmp/cryptobench.txt", "w"); return f ? 1 : -1; }
int fs_write_chunk(int h, const void *b, int n) { (void)h; return (int)fwrite(b, 1, (size_t)n, f); }
int fs_close_handle(int h) { (void)h; fclose(f); return 1; }
