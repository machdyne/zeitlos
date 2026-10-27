/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * tools/learn/rvbench -- learn's platform (lplat.h), for sim.py
 */
#include <stdio.h>
#include <string.h>
#include "lplat.h"
long sim_call(long n, long a, long b, long c, long d);
uint32_t sim_ms(void) { return (uint32_t)sim_call(506, 0, 0, 0, 0); }
static char lbuf[16384];
static bool s_list(void *ctx, const char *dir, void (*fn)(void *, const char *, bool), void *arg) {
	(void)ctx;
	long n = sim_call(500, (long)dir, (long)lbuf, sizeof lbuf, 0);
	if (n < 0) return false;
	for (long i = 0; i < n;) { bool d = lbuf[i] == 'd'; fn(arg, lbuf + i + 1, d); i += strlen(lbuf + i) + 1; }
	return true;
}
static long s_read_at(const char *p, long off, char *buf, long cap) { return sim_call(501, (long)p, off, (long)buf, cap); }
static long s_read(void *c, const char *p, char *b, long cap) { (void)c; return s_read_at(p, 0, b, cap); }
static bool s_ww(const char *p, const char *d, long n) { return sim_call(502, (long)p, (long)d, n, 0); }
static bool s_ap(const char *p, const char *d, long n) { return sim_call(503, (long)p, (long)d, n, 0); }
static bool s_mk(const char *p) { return sim_call(504, (long)p, 0, 0, 0); }
static bool s_ex(const char *p) { return sim_call(505, (long)p, 0, 0, 0); }
static uint32_t s_now(void) { return 1793000000u; }
static void s_speak(const char *t) { (void)t; }
static void s_stop(void) { }
static bool s_prompt(const char *t, const char *m, const char *i, char *o, int cap) { (void)t; (void)m; (void)i; snprintf(o, cap, "Bench"); return true; }
static bool s_conf(const char *t, const char *m) { (void)t; (void)m; return true; }
static bool s_run(const char *a, const char *g) { (void)a; (void)g; return true; }
const lplat_t sim_plat = { { s_list, s_read, NULL }, s_read_at, s_ww, s_ap, s_mk, s_ex, s_now, sim_ms, s_speak, s_stop, s_prompt, s_conf, s_run };
