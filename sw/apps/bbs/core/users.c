/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * bbs -- users: <datadir>/users.dat.
 *
 * Fixed 256-byte records, record i at byte i * 256, the user's id i + 1.
 * Written a field at a time, little-endian, rather than as a struct:
 * the same file must read the same on the board (RV32) and on a Linux
 * server, whatever either compiler does with padding. A record is
 * changed in place (PLAT_UPDATE, one seek, one write), and a new one is
 * appended, so there is never a moment when the file is half-rewritten.
 * docs/bbs.md, "Users".
 *
 * Nothing is ever deleted: a disabled user keeps their record, their
 * id and their handle, so messages that name them keep meaning them.
 */
#include <string.h>
#include "bbs_int.h"
#include "zsha256.h"

#define USERS_FILE "users.dat"
static const uint8_t magic[4] = { 'Z', 'B', 'U', '1' };

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void user_pack(const user_t *u, uint8_t r[USER_REC_SIZE]) {
	memset(r, 0, USER_REC_SIZE);
	memcpy(r, magic, 4);
	put32(r + 4, u->id);
	memcpy(r + 8, u->handle, USER_HANDLE_BYTES);
	r[8 + USER_HANDLE_BYTES - 1] = 0;
	memcpy(r + 72, u->location, USER_LOC_BYTES);
	r[72 + USER_LOC_BYTES - 1] = 0;
	put16(r + 120, u->flags);
	r[122] = u->level;
	r[123] = u->charset;
	r[124] = u->color;
	r[125] = u->rows;
	put32(r + 128, u->pw_iter);
	memcpy(r + 132, u->salt, 16);
	memcpy(r + 148, u->hash, 32);
	put32(r + 180, u->created);
	put32(r + 184, u->last_login);
	put32(r + 188, u->prev_login);
	put32(r + 192, u->calls);
	// 196-255: reserved, zero
}

bool user_unpack(user_t *u, const uint8_t r[USER_REC_SIZE]) {
	memset(u, 0, sizeof(*u));
	if (memcmp(r, magic, 4)) return false;
	u->id = get32(r + 4);
	memcpy(u->handle, r + 8, USER_HANDLE_BYTES);
	u->handle[USER_HANDLE_BYTES - 1] = 0;
	memcpy(u->location, r + 72, USER_LOC_BYTES);
	u->location[USER_LOC_BYTES - 1] = 0;
	u->flags = get16(r + 120);
	u->level = r[122];
	u->charset = r[123] <= CS_ASCII ? r[123] : CS_AUTO;
	u->color = r[124] <= COLOR_OFF ? r[124] : COLOR_AUTO;
	u->rows = r[125];
	u->pw_iter = get32(r + 128);
	memcpy(u->salt, r + 132, 16);
	memcpy(u->hash, r + 148, 32);
	u->created = get32(r + 180);
	u->last_login = get32(r + 184);
	u->prev_login = get32(r + 188);
	u->calls = get32(r + 192);
	return true;
}

static bool users_path(char *p) {
	return bbs_path(p, USERS_FILE);
}

int users_count(void) {
	char p[BBS_PATH_MAX];
	if (!users_path(p)) return -1;
	int32_t sz = plat_size(p);
	if (sz < 0) return 0;
	return (int)(sz / USER_REC_SIZE);
}

bool users_read(int idx, user_t *u) {
	char p[BBS_PATH_MAX];
	uint8_t r[USER_REC_SIZE];
	if (idx < 0 || !users_path(p)) return false;
	int h = plat_open(p, PLAT_READ);
	if (h < 0) return false;
	bool ok = plat_seek(h, (uint32_t)idx * USER_REC_SIZE) &&
		plat_read(h, r, USER_REC_SIZE) == USER_REC_SIZE;
	plat_close(h);
	return ok && user_unpack(u, r);
}

void handle_key(const char *in, char *out, uint32_t cap) {
	uint32_t i = 0;
	for (; in[i] && i + 1 < cap; i++) {
		char c = in[i];
		out[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
	}
	out[i] = 0;
}

// One pass over the file, one handle open for all of it.
int users_find(const char *handle) {
	char p[BBS_PATH_MAX], want[USER_HANDLE_BYTES], have[USER_HANDLE_BYTES];
	uint8_t r[USER_REC_SIZE];
	user_t u;
	int found = -1;
	if (!users_path(p)) return -1;
	handle_key(handle, want, sizeof(want));
	int h = plat_open(p, PLAT_READ);
	if (h < 0) return -1;
	for (int i = 0; plat_read(h, r, USER_REC_SIZE) == USER_REC_SIZE; i++) {
		if (!user_unpack(&u, r)) continue;
		handle_key(u.handle, have, sizeof(have));
		if (!strcmp(want, have)) { found = i; break; }
	}
	plat_close(h);
	return found;
}

bool users_write(const user_t *u) {
	char p[BBS_PATH_MAX];
	uint8_t r[USER_REC_SIZE];
	if (!u->id || !users_path(p)) return false;
	user_pack(u, r);
	int h = plat_open(p, PLAT_UPDATE);
	if (h < 0) return false;
	bool ok = plat_seek(h, (u->id - 1) * USER_REC_SIZE) &&
		plat_write(h, r, USER_REC_SIZE) == USER_REC_SIZE;
	plat_close(h);
	if (!ok) bbs_logf("could not write user %u", (unsigned)u->id);
	return ok;
}

bool users_add(user_t *u) {
	char p[BBS_PATH_MAX];
	uint8_t r[USER_REC_SIZE];
	if (!users_path(p)) return false;
	if (plat_size(p) < 0) {
		int h = plat_open(p, PLAT_CREATE);
		if (h < 0) return false;
		plat_close(h);
	}
	int32_t sz = plat_size(p);
	// A partly written record at the end (a crash mid-append) is
	// overwritten, not left between two good ones.
	uint32_t idx = (uint32_t)sz / USER_REC_SIZE;
	u->id = idx + 1;
	user_pack(u, r);
	int h = plat_open(p, PLAT_UPDATE);
	if (h < 0) return false;
	bool ok = plat_seek(h, idx * USER_REC_SIZE) && plat_write(h, r, USER_REC_SIZE) == USER_REC_SIZE;
	plat_close(h);
	if (!ok) bbs_logf("could not add a user to %s", p);
	return ok;
}

// -- passwords --
//
// SHA-256 of the salt and the password, then that many times again of
// the previous result, the salt and the password. Simple, standard in
// shape, no new code (zsha256), and the iteration count is per user, so
// raising pw_iterations in bbs.cfg applies to passwords set from then
// on without breaking the old ones. docs/bbs.md, "Passwords".

static void pw_hash(const uint8_t salt[16], const char *pw, uint32_t iter, uint8_t out[32]) {
	z_sha256_ctx c;
	uint32_t pl = (uint32_t)strlen(pw);
	z_sha256_init(&c);
	z_sha256_update(&c, salt, 16);
	z_sha256_update(&c, pw, pl);
	z_sha256_final(&c, out);
	for (uint32_t i = 1; i < iter; i++) {
		z_sha256_init(&c);
		z_sha256_update(&c, out, 32);
		z_sha256_update(&c, salt, 16);
		z_sha256_update(&c, pw, pl);
		z_sha256_final(&c, out);
	}
}

void pw_set(user_t *u, const char *pw, int iterations) {
	if (iterations < 1) iterations = 1;
	plat_random(u->salt, sizeof(u->salt));
	u->pw_iter = (uint32_t)iterations;
	pw_hash(u->salt, pw, u->pw_iter, u->hash);
}

bool pw_check(const user_t *u, const char *pw) {
	uint8_t h[32];
	uint8_t diff = 0;
	if (!u->pw_iter) return false;          // no password set: nothing matches
	pw_hash(u->salt, pw, u->pw_iter, h);
	for (int i = 0; i < 32; i++) diff |= (uint8_t)(h[i] ^ u->hash[i]);
	return diff == 0;
}

// -- handles --

const char *handle_problem(const char *h) {
	const char *end = h + strlen(h), *p = h;
	int chars = 0;
	char key[USER_HANDLE_BYTES];
	if (!*h) return "a handle cannot be empty";
	if (h[0] == ' ' || end[-1] == ' ') return "no spaces at the start or end";
	uint32_t prev = 0;
	while (p < end) {
		uint32_t cp = utf8_next(&p, end);
		chars++;
		if (cp == 0xFFFD) return "that is not valid text";
		if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp < 0xA0)) return "no control characters";
		if (cp == '@') return "no @ -- it joins a handle to its BBS";
		if (cp == '|') return "no | -- it starts a colour code";
		if (cp == ' ' && prev == ' ') return "one space at a time";
		prev = cp;
	}
	if (chars < 2) return "at least 2 characters";
	if (chars > BBS_HANDLE_MAX) return "at most 20 characters";
	if (end - h >= USER_HANDLE_BYTES) return "too long";
	handle_key(h, key, sizeof(key));
	if (!strcmp(key, "new") || !strcmp(key, "all") || !strcmp(key, "everyone"))
		return "that word is reserved";
	return NULL;
}
