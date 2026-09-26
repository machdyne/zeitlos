#ifndef Z_AUTH_H
#define Z_AUTH_H
/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The machine's password, and the screen-lock policy: Z_SYS_AUTH.
 * docs/security.md.
 *
 * Zeitlos is single-user, and this is that user's one password. The
 * kernel keeps it -- a salted PBKDF2-HMAC-SHA256 hash in the flash
 * key/value store (docs/kvstore.md), so it exists with or without an
 * sdcard -- and checks it. Nothing here ever hands the hash out.
 *
 * Checking takes about half a second of CPU inside the call: the hash
 * is made slow on purpose, and the iteration count is set so that it
 * costs that much on this machine. Show the user something before
 * calling.
 *
 * Every check anywhere counts toward one system-wide tally. After
 * three failures in a row the kernel refuses further checks for 1 s,
 * then 2, 4, ... up to 60 s, and says how long in wait_ms. A success
 * resets it. One check runs at a time; a second one arriving meanwhile
 * gets Z_AUTH_E_WAIT too.
 */
#include <stdint.h>
#include "zeitlos.h"
#include "zobj.h"
#include "zwm.h"

#define Z_AUTH_PW_MAX       64      // bytes; any bytes but NUL
#define Z_AUTH_NET_MIN      10      // the shortest password network logins accept

// results
#define Z_AUTH_OK            0
#define Z_AUTH_E_BAD        -1      // wrong password
#define Z_AUTH_E_WAIT       -2      // too many failures, or a check in progress: wait_ms
#define Z_AUTH_E_NOPASS     -3      // no password is set (CHECK)
#define Z_AUTH_E_INVAL      -4      // empty, too long, or a bad policy value
#define Z_AUTH_E_STORE      -5      // the key/value store refused (read-only bitstream, full)
#define Z_AUTH_E_NOSYS      -9      // the running kernel has no Z_SYS_AUTH

enum {
	Z_AUTH_STATUS = 0,
	Z_AUTH_CHECK = 1,
	Z_AUTH_SET = 2,         // pw: current (ignored if none is set); newpw: new, or newlen 0 to remove
	Z_AUTH_POLICY = 3,      // pw: current (if one is set); st: the lock_* fields to store
};

// status flags
#define Z_AUTH_HAS_PASSWORD  1u
#define Z_AUTH_WRITABLE      2u     // the password and policy can be changed on this bitstream
#define Z_AUTH_NET_OK        4u     // the password is at least Z_AUTH_NET_MIN long

typedef struct {
	uint32_t flags;         // Z_AUTH_HAS_PASSWORD etc.
	uint32_t lock_boot;     // 1: wm starts locked
	uint32_t lock_idle_min; // lock after this many minutes without input; 0 never
	uint32_t lock_console;  // 1: the serial console asks for the password too
	uint32_t failures;      // consecutive failed checks
	uint32_t wait_ms;       // until the next check is allowed
	uint32_t iterations;    // of the stored hash
} z_auth_status_t;

typedef struct {
	uint32_t op;
	const char *pw;
	uint32_t pwlen;
	const char *newpw;
	uint32_t newlen;
	z_auth_status_t *st;    // STATUS: out. POLICY: in.
	int32_t result;
	uint32_t wait_ms;       // with Z_AUTH_E_WAIT: how long
} z_auth_args_t;

static inline int z_auth_call(z_auth_args_t *a) {
	z_kernel_ptr_t k = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	a->result = Z_AUTH_E_NOSYS;
	a->wait_ms = 0;
	k(Z_SYS_AUTH, (uint32_t *)a, 0);
	return a->result;
}

static inline int z_auth_status(z_auth_status_t *st) {
	z_auth_args_t a = { Z_AUTH_STATUS, 0, 0, 0, 0, st, 0, 0 };
	return z_auth_call(&a);
}

// Z_AUTH_OK if `pw` is the password. *wait_ms (may be NULL) is set with
// Z_AUTH_E_WAIT.
static inline int z_auth_check(const char *pw, uint32_t len, uint32_t *wait_ms) {
	z_auth_args_t a = { Z_AUTH_CHECK, pw, len, 0, 0, 0, 0, 0 };
	int rc = z_auth_call(&a);
	if (wait_ms) *wait_ms = a.wait_ms;
	return rc;
}

// Sets, changes (newlen > 0) or removes (newlen 0) the password. `cur`
// must be the current one if there is one.
static inline int z_auth_set(const char *cur, uint32_t curlen,
		const char *newpw, uint32_t newlen, uint32_t *wait_ms) {
	z_auth_args_t a = { Z_AUTH_SET, cur, curlen, newpw, newlen, 0, 0, 0 };
	int rc = z_auth_call(&a);
	if (wait_ms) *wait_ms = a.wait_ms;
	return rc;
}

// Stores st->lock_boot, lock_idle_min (0..1440) and lock_console.
static inline int z_auth_policy(const char *cur, uint32_t curlen,
		const z_auth_status_t *st, uint32_t *wait_ms) {
	z_auth_args_t a = { Z_AUTH_POLICY, cur, curlen, 0, 0, (z_auth_status_t *)st, 0, 0 };
	int rc = z_auth_call(&a);
	if (wait_ms) *wait_ms = a.wait_ms;
	return rc;
}

// Asks wm to lock the screen (Z_WM_LOCK_NOW) or to re-read the policy
// (Z_WM_LOCK_RELOAD). False if wm is not running.
static inline bool z_wm_lock_request(uint32_t what) {
	uint32_t wm;
	if (!z_pid_lookup("wm0", &wm)) return false;
	return z_msg_new_send(wm, Z_WM_LOCK, 0, z_obj_uint32(what)) == Z_OK;
}

#endif
