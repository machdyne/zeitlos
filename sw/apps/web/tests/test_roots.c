/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Checks a trust store built by tools/mkroots.py: every index entry
 * must point at a certificate that parses, whose subject DN hashes to
 * the value in the table, in a table that is sorted.
 *
 *   make test-roots ROOTS_PEM=/etc/ssl/certs/ca-certificates.crt
 *
 * The index is a CLAIM ABOUT THE FILE, and a wrong one does not fail
 * loudly on the device: it produces "the chain does not lead to a
 * known root" for one site and works perfectly for every other. That
 * is the hardest kind of trust bug to notice, and the cheapest to
 * rule out here -- this parses with the same x509.c the device uses.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../der.h"
#include "../x509.h"
#include "../../../common/zsha256.h"
int main(int argc, char **argv) {
	FILE *f = fopen(argv[1], "rb");
	static uint8_t buf[4*1024*1024];
	uint32_t n = fread(buf, 1, sizeof buf, f), count, i, bad = 0;
	if (memcmp(buf, "ZROOTS\0\0", 8)) { printf("FAIL: no magic\n"); return 1; }
	count = buf[12] | (buf[13]<<8) | (buf[14]<<16) | ((uint32_t)buf[15]<<24);
	printf("index claims %u certificates\n", count);
	for (i = 0; i < count; i++) {
		const uint8_t *e = buf + 24 + 16*i;
		uint32_t off = e[8] | (e[9]<<8) | (e[10]<<16) | ((uint32_t)e[11]<<24);
		uint32_t len = e[12] | (e[13]<<8) | (e[14]<<16) | ((uint32_t)e[15]<<24);
		x509_cert_t c; const char *err = NULL; uint8_t h[32];
		if (off + len > n) { printf("FAIL %u: past end\n", i); bad++; continue; }
		if (!x509_parse(buf + off, len, &c, &err)) {
			printf("FAIL %u: unparseable (%s)\n", i, err?err:"?"); bad++; continue; }
		z_sha256(h, c.subject.p, c.subject.len);
		if (memcmp(h, e, 8)) { printf("FAIL %u: dn hash mismatch\n", i); bad++; }
		if (i && memcmp(e - 16, e, 8) > 0) {
			printf("FAIL %u: table not sorted\n", i); bad++; }
	}
	printf("%s: %u entries, %u bad\n", bad?"FAIL":"ok", count, bad);
	return bad ? 1 : 0;
}
