/*
 * Zeitlos OS
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The password and the lock policy, the kernel's side. auth.h;
 * docs/security.md is the design and the threat model.
 *
 * -- what is stored --
 *
 *   auth.password     authcore.h's record: a salted PBKDF2-HMAC-SHA256
 *                     key and its iteration count, 54 bytes
 *   sys.lock.boot     "1": wm starts locked
 *   sys.lock.idle     minutes without input before wm locks, "1".."1440"
 *   sys.lock.console  "1": the serial console asks for the password
 *
 * All in the flash key/value store, where apps can list them but not
 * read or write them (kvstore.c); only this file writes them, and
 * only after the current password has been given.
 *
 * -- what is enforced here, for everyone --
 *
 * One tally of consecutive failures and one "not before" time, shared
 * by every caller: wm's lock screen, the console, and later netserve.
 * One check runs at a time -- the gate -- so parallel callers cannot
 * multiply the guessing rate. A process killed while holding the gate
 * releases it through the reaper (k_auth_release_pid()).
 *
 * -- the iteration count --
 *
 * Measured, not fixed: when a password is set, PBKDF2 is timed on this
 * machine and the count chosen to take about half a second. That is a
 * few hundred iterations on a picorv32 -- which is why the online
 * backoff matters more here than the hash does. docs/security.md says
 * so plainly.
 */
#include <stdio.h>
#include <string.h>

#include "kernel.h"
#include "kvstore.h"
#include "authcore.h"
#include "auth.h"
#include "uart.h"
#include "pidreg.h"
#include "../common/zsoc.h"
#include "../common/zrng.h"
#include "../common/zkv.h"

#define KEY_PW       "auth.password"
#define KEY_BOOT     "sys.lock.boot"
#define KEY_IDLE     "sys.lock.idle"
#define KEY_CONSOLE  "sys.lock.console"

#define AUTH_TARGET_TICKS  (Z_TICK_HZ / 2)       // ~0.5 s per check
#define AUTH_ITER_MIN      64u
#define AUTH_ITER_MAX      (1u << 20)
#define AUTH_IDLE_MAX      1440u                 // a day, in minutes

#define BSS __attribute__((section(".bss")))
static uint32_t BSS fails;          // consecutive failures, all callers
static uint32_t BSS not_before;     // tick before which checks are refused
static uint32_t BSS gate;           // holder pid + 1; 0 free

static bool gate_take(void) {
	bool ok;
	k_fs_enter();
	ok = (gate == 0);
	if (ok) gate = z_pid + 1;
	k_fs_leave();
	return ok;
}

static void gate_give(void) {
	gate = 0;
}

void k_auth_release_pid(uint32_t pid) {
	if (gate == pid + 1) gate = 0;
}

static void wipe(void *p, uint32_t n) {
	volatile uint8_t *v = (volatile uint8_t *)p;
	while (n--) *v++ = 0;
}

static bool load(auth_rec_t *r) {
	uint8_t b[AUTH_REC_LEN];
	uint32_t n = 0;
	bool ok = k_kv_get(KEY_PW, b, sizeof(b), &n) == KV_OK &&
		auth_rec_unpack(b, n, r) == 0;
	wipe(b, sizeof(b));
	return ok;
}

static uint32_t wait_left_ms(void) {
	int32_t left = (int32_t)(not_before - z_kernel_ticks);
	if (left <= 0) return 0;
	return (uint32_t)left * 1000u / Z_TICK_HZ + 1;
}

// -- the check: the gate must be held --

static int verify(const char *who, const uint8_t *pw, uint32_t len, uint32_t *wait_ms) {
	auth_rec_t r;
	uint8_t dk[AUTH_DK_LEN];
	uint32_t w = wait_left_ms(), ms;
	int ok;

	if (w) { if (wait_ms) *wait_ms = w; return Z_AUTH_E_WAIT; }
	if (!load(&r)) return Z_AUTH_E_NOPASS;
	if (!len) return Z_AUTH_E_BAD;

	auth_pbkdf2(pw, len, r.salt, AUTH_SALT_LEN, r.iterations, dk);
	ok = auth_eq(dk, r.dk, AUTH_DK_LEN);
	wipe(dk, sizeof(dk));
	wipe(&r, sizeof(r));

	if (ok) {
		fails = 0;
		return Z_AUTH_OK;
	}
	fails++;
	ms = auth_backoff_ms(fails);
	if (ms) not_before = z_kernel_ticks + ms / 1000u * Z_TICK_HZ + (ms % 1000u) * Z_TICK_HZ / 1000u;
	printf("auth: wrong password from %s, %lu in a row", who, (unsigned long)fails);
	if (ms) printf("; next check in %lu s", (unsigned long)(ms / 1000u));
	printf("\n");
	return Z_AUTH_E_BAD;
}

// -- setting it --

// 16 bytes that need to be unique rather than secret. From the TRNG
// when there is one (docs/trng.md), hashed together with the time and
// the cycle counter, which on their own would do on a board without.
static void make_salt(uint8_t salt[AUTH_SALT_LEN]) {
	z_sha256_ctx c;
	uint8_t h[Z_SHA256_DIGEST];
	uint32_t w, got = 0, t0 = z_kernel_ticks;

	z_sha256_init(&c);
	if (z_rng_present())
		while (got < 8 && z_kernel_ticks - t0 < Z_TICK_HZ / 10)
			if (z_rng_hw_word(&w)) { z_sha256_update(&c, &w, 4); got++; }
	w = z_kernel_ticks;
	z_sha256_update(&c, &w, 4);
	__asm__ volatile ("rdcycle %0" : "=r"(w));
	z_sha256_update(&c, &w, 4);
	z_sha256_final(&c, h);
	memcpy(salt, h, AUTH_SALT_LEN);
}

// Iterations that take AUTH_TARGET_TICKS here, doubling a trial run
// until it is long enough to time.
static uint32_t calibrate(void) {
	static const uint8_t salt[AUTH_SALT_LEN] = "zeitlos-calibra";
	uint8_t dk[AUTH_DK_LEN];
	uint32_t n = 16, t;

	for (;;) {
		uint32_t t0 = z_kernel_ticks;
		auth_pbkdf2((const uint8_t *)"calibrate", 9, salt, AUTH_SALT_LEN, n, dk);
		t = z_kernel_ticks - t0;
		if (t >= Z_TICK_HZ / 20 || n >= AUTH_ITER_MAX) break;
		n <<= 1;
	}
	if (!t) t = 1;
	n = n * AUTH_TARGET_TICKS / t;
	if (n < AUTH_ITER_MIN) n = AUTH_ITER_MIN;
	if (n > AUTH_ITER_MAX) n = AUTH_ITER_MAX;
	return n;
}

static int store_new(const uint8_t *pw, uint32_t len) {
	auth_rec_t r;
	uint8_t b[AUTH_REC_LEN];
	int rc;

	make_salt(r.salt);
	r.iterations = calibrate();
	r.flags = len >= Z_AUTH_NET_MIN ? AUTH_REC_NET_OK : 0;
	auth_pbkdf2(pw, len, r.salt, AUTH_SALT_LEN, r.iterations, r.dk);
	auth_rec_pack(&r, b);
	// Scrubbed: the old hash is erased from the flash, not superseded.
	rc = k_kv_set(KEY_PW, b, sizeof(b), KV_SCRUB);
	wipe(b, sizeof(b));
	wipe(&r, sizeof(r));
	return rc == KV_OK ? Z_AUTH_OK : Z_AUTH_E_STORE;
}

static int put_num(const char *key, uint32_t v) {
	char s[12];
	int n = 0, rc;
	uint32_t d = 1000000000u;
	if (!v) {
		rc = k_kv_del(key, 0);
		return (rc == KV_OK || rc == KV_ENOENT) ? 0 : -1;
	}
	for (; d; d /= 10)
		if (v >= d || n) { s[n++] = (char)('0' + v / d % 10); }
	return k_kv_set(key, s, (uint32_t)n, 0) == KV_OK ? 0 : -1;
}

static uint32_t get_num(const char *key) {
	char s[12];
	uint32_t n = 0, v = 0, i;
	if (k_kv_get(key, s, sizeof(s), &n) != KV_OK) return 0;
	for (i = 0; i < n && s[i] >= '0' && s[i] <= '9'; i++) v = v * 10 + (uint32_t)(s[i] - '0');
	return v;
}

// -- the operations, shared by the syscall and the console --

static int do_check(const char *who, const uint8_t *pw, uint32_t len, uint32_t *w) {
	int rc;
	if (!gate_take()) { *w = 250; return Z_AUTH_E_WAIT; }
	rc = verify(who, pw, len, w);
	gate_give();
	return rc;
}

static int do_set(const char *who, const uint8_t *cur, uint32_t curlen,
		const uint8_t *npw, uint32_t newlen, uint32_t *w) {
	auth_rec_t r;
	bool has;
	int rc = Z_AUTH_OK;

	if (!k_kv_writable()) return Z_AUTH_E_STORE;
	if (!gate_take()) { *w = 250; return Z_AUTH_E_WAIT; }
	has = load(&r);
	wipe(&r, sizeof(r));
	if (has) rc = verify(who, cur, curlen, w);
	if (rc == Z_AUTH_OK) {
		if (newlen) rc = store_new(npw, newlen);
		else if (has) rc = k_kv_del(KEY_PW, KV_SCRUB) == KV_OK ? Z_AUTH_OK : Z_AUTH_E_STORE;
		if (rc == Z_AUTH_OK && (newlen || has))
			printf("auth: password %s by %s\n", !newlen ? "removed" : has ? "changed" : "set", who);
	}
	gate_give();
	return rc;
}

static int do_policy(const char *who, const uint8_t *cur, uint32_t curlen,
		const z_auth_status_t *p, uint32_t *w) {
	auth_rec_t r;
	int rc = Z_AUTH_OK;

	if (p->lock_boot > 1 || p->lock_console > 1 || p->lock_idle_min > AUTH_IDLE_MAX)
		return Z_AUTH_E_INVAL;
	if (!k_kv_writable()) return Z_AUTH_E_STORE;
	if (!gate_take()) { *w = 250; return Z_AUTH_E_WAIT; }
	if (load(&r)) rc = verify(who, cur, curlen, w);
	wipe(&r, sizeof(r));
	if (rc == Z_AUTH_OK) {
		if (put_num(KEY_BOOT, p->lock_boot) || put_num(KEY_IDLE, p->lock_idle_min) ||
				put_num(KEY_CONSOLE, p->lock_console))
			rc = Z_AUTH_E_STORE;
		else
			printf("auth: lock policy set by %s: boot %s, idle %lu min, console %s\n", who,
				p->lock_boot ? "yes" : "no", (unsigned long)p->lock_idle_min,
				p->lock_console ? "yes" : "no");
	}
	gate_give();
	return rc;
}

void k_auth_status(z_auth_status_t *st) {
	auth_rec_t r;
	memset(st, 0, sizeof(*st));
	if (load(&r)) {
		st->flags |= Z_AUTH_HAS_PASSWORD;
		if (r.flags & AUTH_REC_NET_OK) st->flags |= Z_AUTH_NET_OK;
		st->iterations = r.iterations;
	}
	wipe(&r, sizeof(r));
	if (k_kv_writable()) st->flags |= Z_AUTH_WRITABLE;
	st->lock_boot = get_num(KEY_BOOT) ? 1 : 0;
	st->lock_idle_min = get_num(KEY_IDLE);
	if (st->lock_idle_min > AUTH_IDLE_MAX) st->lock_idle_min = AUTH_IDLE_MAX;
	st->lock_console = get_num(KEY_CONSOLE) ? 1 : 0;
	st->failures = fails;
	st->wait_ms = wait_left_ms();
}

void k_auth_init(void) {
	z_auth_status_t st;
	fails = 0;
	not_before = 0;
	gate = 0;
	k_auth_status(&st);
	if (!(st.flags & Z_AUTH_HAS_PASSWORD)) {
		printf(" - auth: no password set\n");
		return;
	}
	printf(" - auth: password set (%lu iterations); lock: boot %d, idle %lu min, console %d\n",
		(unsigned long)st.iterations, (int)st.lock_boot, (unsigned long)st.lock_idle_min,
		(int)st.lock_console);
}

// -- Z_SYS_AUTH --

static void caller_name(char *out, uint32_t n) {
	const char *name = k_pidreg_name_for(z_pid);
	snprintf(out, n, "%s (pid %lu)", name ? name : "an app", (unsigned long)z_pid);
}

z_obj_t *k_auth(z_obj_t *args) {
	z_auth_args_t *a = (z_auth_args_t *)args;
	uint8_t pw[Z_AUTH_PW_MAX], npw[Z_AUTH_PW_MAX];
	uint32_t w = 0, pwlen, newlen;
	z_auth_status_t pol;
	char who[32];
	int rc;

	if (!a || !k_user_ok_words(a, sizeof(*a))) return &z_fail;
	pwlen = a->pwlen;
	newlen = a->newlen;
	// Copied in before use: the app's buffers could change under a
	// half-second check, and the copies are wiped on the way out.
	if (pwlen > Z_AUTH_PW_MAX || newlen > Z_AUTH_PW_MAX ||
			(pwlen && !a->pw) || (newlen && !a->newpw)) {
		rc = Z_AUTH_E_INVAL;
		goto out;
	}
	if (pwlen) memcpy(pw, a->pw, pwlen);
	if (newlen) memcpy(npw, a->newpw, newlen);
	caller_name(who, sizeof(who));

	switch (a->op) {
	case Z_AUTH_STATUS:
		if (!a->st || !k_user_ok_words(a->st, sizeof(*a->st))) { rc = Z_AUTH_E_INVAL; break; }
		k_auth_status(a->st);
		rc = Z_AUTH_OK;
		break;
	case Z_AUTH_CHECK:
		rc = do_check(who, pw, pwlen, &w);
		break;
	case Z_AUTH_SET:
		rc = do_set(who, pw, pwlen, npw, newlen, &w);
		break;
	case Z_AUTH_POLICY:
		if (!a->st || !k_user_ok_words(a->st, sizeof(*a->st))) { rc = Z_AUTH_E_INVAL; break; }
		pol = *a->st;
		rc = do_policy(who, pw, pwlen, &pol, &w);
		break;
	default:
		rc = Z_AUTH_E_INVAL;
	}
out:
	wipe(pw, sizeof(pw));
	wipe(npw, sizeof(npw));
	a->result = rc;
	a->wait_ms = w;
	return rc == Z_AUTH_OK ? &z_ok : &z_fail;
}

// -- the console --

// A line, echoed or not. Ctrl+C gives -1. *all_uart (may be NULL) is
// cleared if any byte of it came through console0 rather than UART0.
// A LF straight after a CR is the second half of one Enter.
static int read_input(const char *prompt, char *buf, uint32_t cap, bool echo,
		bool *all_uart) {
	static char prev;
	uint32_t n = 0;
	int c;

	printf("%s", prompt);
	fflush(stdout);
	for (;;) {
		while (k_uart_rx_empty()) k_uart_wait_rx();
		c = k_uart_getc();
		if (c < 0) continue;
		if (k_uart_last_injected && all_uart) *all_uart = false;
		if (c == '\n' && prev == '\r' && n == 0) { prev = 0; continue; }
		prev = (char)c;
		if (c == '\r' || c == '\n') break;
		if (c == 0x03) { printf("^C\n"); wipe(buf, cap); return -1; }
		if (c == 0x08 || c == 0x7f) {
			if (n) { n--; if (echo) printf("\b \b"); fflush(stdout); }
			continue;
		}
		if ((uint8_t)c < 0x20 || n + 1 >= cap) continue;
		buf[n++] = (char)c;
		if (echo) { printf("%c", c); fflush(stdout); }
	}
	buf[n] = 0;
	printf("\n");
	return (int)n;
}

static void report(const char *what, int rc, uint32_t w) {
	switch (rc) {
	case Z_AUTH_E_BAD:   printf("%s: wrong password\n", what); break;
	case Z_AUTH_E_WAIT:  printf("%s: wait %lu s\n", what,
		(unsigned long)((w + 999) / 1000)); break;
	case Z_AUTH_E_STORE: printf("%s: the flash store refused (kv)\n", what); break;
	case Z_AUTH_E_INVAL: printf("%s: %d characters at most\n", what, Z_AUTH_PW_MAX); break;
	default: break;
	}
}

static void console_unlock(void) {
	char pw[Z_AUTH_PW_MAX + 1];
	uint32_t w;
	int n, rc;

	printf("\nthe console is locked.\n");
	for (;;) {
		n = read_input("password: ", pw, sizeof(pw), false, NULL);
		if (n <= 0) continue;
		w = 0;
		rc = do_check("the console", (const uint8_t *)pw, (uint32_t)n, &w);
		wipe(pw, sizeof(pw));
		if (rc == Z_AUTH_OK || rc == Z_AUTH_E_NOPASS) break;
		report("unlock", rc, w);
	}
	printf("unlocked.\n");
}

void k_auth_console_boot(void) {
	z_auth_status_t st;
	k_auth_status(&st);
	if ((st.flags & Z_AUTH_HAS_PASSWORD) && st.lock_console) console_unlock();
}

void k_auth_shell_lock(void) {
	z_auth_status_t st;
	bool wm = z_wm_lock_request(Z_WM_LOCK_NOW);

	k_auth_status(&st);
	if (!(st.flags & Z_AUTH_HAS_PASSWORD)) {
		printf("lock: no password set (passwd)\n");
		return;
	}
	if (wm) printf("lock: the screen is locked\n");
	if (st.lock_console) console_unlock();
	else if (!wm) printf("lock: no wm, and the console lock is off\n");
}

static void passwd_reset(bool line_from_uart) {
	char buf[8];
	bool uart = line_from_uart;

	// Someone at the machine, holding a serial cable, can reflash it
	// anyway: this is how a forgotten password is recovered. From a
	// term window it would be a way around the password.
	if (!uart) {
		printf("passwd reset: serial console (UART0) only\n");
		return;
	}
	if (read_input("remove the password and lock settings? type RESET: ", buf, sizeof(buf), true, &uart) < 0 ||
			strcmp(buf, "RESET") || !uart) {
		printf("passwd reset: nothing changed\n");
		return;
	}
	k_kv_del(KEY_PW, KV_SCRUB);
	k_kv_del(KEY_BOOT, 0);
	k_kv_del(KEY_IDLE, 0);
	k_kv_del(KEY_CONSOLE, 0);
	fails = 0;
	not_before = 0;
	printf("passwd: password and lock settings removed\n");
	z_wm_lock_request(Z_WM_LOCK_RELOAD);
}

void k_auth_shell_passwd(const char *sub, bool line_from_uart) {
	char cur[Z_AUTH_PW_MAX + 1], n1[Z_AUTH_PW_MAX + 1], n2[Z_AUTH_PW_MAX + 1];
	int lc = 0, l1, rc;
	uint32_t w = 0;
	auth_rec_t r;
	bool has;

	if (sub && !strcmp(sub, "reset")) { passwd_reset(line_from_uart); return; }
	if (sub) {
		printf("usage: passwd [reset]\n");
		return;
	}
	if (!k_kv_writable()) {
		printf("passwd: this bitstream cannot write the flash\n");
		return;
	}
	has = load(&r);
	wipe(&r, sizeof(r));

	if (has && (lc = read_input("current password: ", cur, sizeof(cur), false, NULL)) < 0)
		return;
	l1 = read_input(has ? "new password (empty to remove it): " : "new password: ",
		n1, sizeof(n1), false, NULL);
	if (l1 < 0 || (l1 == 0 && !has)) {
		printf("passwd: nothing changed\n");
		goto done;
	}
	if (l1 > 0) {
		if (read_input("again: ", n2, sizeof(n2), false, NULL) != l1 || strcmp(n1, n2)) {
			printf("passwd: the two differ -- nothing changed\n");
			goto done;
		}
		if (l1 < Z_AUTH_NET_MIN)
			printf("passwd: under %d characters -- network logins will refuse it\n",
				Z_AUTH_NET_MIN);
	}
	printf("passwd: working...\n");
	rc = do_set("the console", (const uint8_t *)cur, (uint32_t)lc,
		(const uint8_t *)n1, (uint32_t)l1, &w);
	if (rc == Z_AUTH_OK) {
		printf("passwd: password %s\n", !l1 ? "removed" : has ? "changed" : "set");
		z_wm_lock_request(Z_WM_LOCK_RELOAD);
	} else
		report("passwd", rc, w);
done:
	wipe(cur, sizeof(cur));
	wipe(n1, sizeof(n1));
	wipe(n2, sizeof(n2));
}
