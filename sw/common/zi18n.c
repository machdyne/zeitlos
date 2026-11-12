/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Languages: the lookup behind Z_() -- see zi18n.h and docs/i18n.md --
 * and finding what jfont holds (zlang.h), which zgfx.c's glyph drawing
 * shares.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "zi18n.h"
#include "zlang.h"
#include "zjfont.h"		// z_zfn_glyph()
#include "zutf8.h"


// -- what jfont holds (zlang.h) --
//
// Found once with z_proc_list() and kept; re-checked (two loads) before
// each use. Until it is found the search is retried every two seconds,
// not on every call, so a machine set to a language whose set is not
// installed costs a process list per two seconds, not per string.

#ifndef Z_HOST_TEST
#include "zeitlos.h"
#include "zcfg.h"

#define LD_RETRY_TICKS  1464		// ~2 s at the ~732Hz tick

static const volatile z_lang_dir_t *ld_dir;
static uint32_t ld_phys;
static uint32_t ld_retry;
static bool ld_retry_armed;

static const volatile z_lang_dir_t *lang_dir(void) {

	if (ld_dir) {
		if (z_lang_dir_ok(ld_dir, ld_phys)) return ld_dir;
		ld_dir = 0;
	}

	uint32_t now = z_uptime_ticks();
	if (ld_retry_armed && (int32_t)(now - ld_retry) < 0) return 0;
	ld_retry = now + LD_RETRY_TICKS;
	ld_retry_armed = true;

	// Every slot (Z_PROCS_MAX): the list is in slot order.
	z_proc_info_t procs[32];
	uint32_t truncated = 0;
	uint32_t n = z_proc_list(procs, 32, &truncated);
	uint32_t at;
	const volatile z_lang_dir_t *d = z_lang_find(procs, n, &at);
	if (d) { ld_dir = d; ld_phys = at; }
	return d;

}
#endif

// Host tests and the simulator hand data over directly.
static const uint8_t *lu_data[Z_LANG_FONT_JP12 + 1];
static uint32_t lu_len[Z_LANG_FONT_JP12 + 1];

void z_lang_use(uint32_t kind, const uint8_t *data, uint32_t len) {
	if (kind > Z_LANG_FONT_JP12) return;
	lu_data[kind] = data;
	lu_len[kind] = len;
}

const uint8_t *z_lang_data(uint32_t kind, uint32_t *len) {
	if (kind <= Z_LANG_FONT_JP12 && lu_data[kind]) {
		if (len) *len = lu_len[kind];
		return lu_data[kind];
	}
#ifndef Z_HOST_TEST
	const volatile z_lang_dir_t *d = lang_dir();
	if (d) {
		uint32_t l = 0, a = z_lang_entry(d, kind, &l);
		if (a) {
			if (len) *len = l;
			return (const uint8_t *)(uintptr_t)a;
		}
	}
#endif
	return 0;
}

const char *z_lang_current(void) {
#ifndef Z_HOST_TEST
	static char cur[8];
	const volatile z_lang_dir_t *d = lang_dir();
	if (d && d->lang[0]) {
		for (int i = 0; i < 7; i++) cur[i] = d->lang[i];
		cur[7] = 0;
		return cur;
	}
#endif
	return "en";
}

// The small build (Z_SMALL, Obst) speaks English: Z_() is the plain
// string there (zi18n.h), and nothing below is compiled. What jfont
// holds is still found, above, so Japanese draws there as it always did.
#ifndef Z_TR_ENGLISH_ONLY

// -- the catalog --

static uint32_t rd32(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// The domain's row in a catalog, or 0; *ndp, the catalog's domains.
static const uint8_t *dom_row(const uint8_t *cat, uint32_t len, uint32_t dom, uint32_t *ndp) {

	if (!cat || len < 24 || memcmp(cat, Z_TR_MAGIC, 4)) return 0;
	uint32_t nd = rd32(cat + 16);
	if (nd > (len - 24) / 12) return 0;
	int lo = 0, hi = (int)nd - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		uint32_t h = rd32(cat + 24 + 12 * mid);
		if (h == dom) { *ndp = nd; return cat + 24 + 12 * mid; }
		if (h < dom) lo = mid + 1; else hi = mid - 1;
	}
	return 0;

}

const char *z_tr_find(const uint8_t *cat, uint32_t len, uint32_t dom, uint32_t hash) {

	uint32_t nd = 0;
	const uint8_t *dp = dom_row(cat, len, dom, &nd);
	if (!dp) return 0;
	int lo, hi;

	uint32_t first = rd32(dp + 4), count = rd32(dp + 8);
	uint32_t ebase = 24 + 12 * nd;
	if (first > len / 8 || count > len / 8 || ebase > len ||
		first + count > (len - ebase) / 8) return 0;

	// the key
	lo = (int)first; hi = (int)(first + count) - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		const uint8_t *e = cat + ebase + 8 * mid;
		uint32_t h = rd32(e);
		if (h == hash) {
			uint32_t off = rd32(e + 4);
			return off < len ? (const char *)cat + off : 0;
		}
		if (h < hash) lo = mid + 1; else hi = mid - 1;
	}
	return 0;

}

// -- the program's state --
//
// TR_NONE: z_tr_init() not called -- English, final.
// TR_EN:   English, final.
// TR_WAIT: another language, its catalog not found yet -- English for
//          now, not cached, looked for again (every two seconds).
// TR_READY: the catalog is in hand.

#define TR_NONE   0
#define TR_EN     1
#define TR_WAIT   2
#define TR_READY  3

static uint8_t tr_state;
static uint8_t tr_rule;
static char tr_lang[8] = "en";
static uint32_t tr_dom_app, tr_dom_common;
// An app's translations are its own catalog (/data/<app>/lang), the
// shared library's another (/data/jfont/lang); jfont holds them all.
static const uint8_t *tr_cat, *tr_cat_c;
static uint32_t tr_len, tr_len_c;

static void set_domains(const char *app) {
	tr_dom_app = z_tr_hash(app && app[0] ? app : "common");
	tr_dom_common = z_tr_hash("common");
}

void z_tr_use(const uint8_t *cat, uint32_t len, const char *lang, const char *app) {
	set_domains(app);
	tr_cat = 0;
	strncpy(tr_lang, lang && lang[0] ? lang : "en", sizeof(tr_lang) - 1);
	tr_lang[sizeof(tr_lang) - 1] = 0;
	if (!strcmp(tr_lang, "en") || !cat) {
		tr_state = TR_EN;
		return;
	}
	z_lang_use(Z_LANG_CATALOG, cat, len);
	tr_state = TR_WAIT;
}

void z_tr_init(const char *app) {

	set_domains(app);
#ifndef Z_HOST_TEST
	char lang[16];
	z_cfg_get_args_t a;
	memset(&a, 0, sizeof(a));
	lang[0] = 0;
	a.key = "system.language";
	a.val = lang;
	a.vallen = sizeof(lang);
	z_kernel_ptr_t z_kernel_ptr = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	z_obj_t *rv = (z_obj_t *)z_kernel_ptr(Z_SYS_CFG_GET, (uint32_t *)&a, 0);
	if (!rv || rv->val.uint32 != Z_OK || !a.found || !lang[0] || !strcmp(lang, "en")) {
		tr_state = TR_EN;
		return;
	}
	strncpy(tr_lang, lang, sizeof(tr_lang) - 1);
	tr_lang[sizeof(tr_lang) - 1] = 0;
	tr_state = TR_WAIT;
#else
	(void)app;
	tr_state = TR_EN;
#endif

}

const char *z_tr_lang(void) {
	return tr_state >= TR_WAIT ? tr_lang : "en";
}

// The catalog that has domain `dom`: the one handed over (host tests),
// or any of jfont's. *seen: the catalogs could be looked at at all.
static const uint8_t *with_domain(uint32_t dom, uint32_t *len, bool *seen) {
	uint32_t nd;
	if (lu_data[Z_LANG_CATALOG]) {
		*seen = true;
		if (!dom_row(lu_data[Z_LANG_CATALOG], lu_len[Z_LANG_CATALOG], dom, &nd)) return 0;
		*len = lu_len[Z_LANG_CATALOG];
		return lu_data[Z_LANG_CATALOG];
	}
#ifndef Z_HOST_TEST
	const volatile z_lang_dir_t *d = lang_dir();
	if (!d) return 0;
	*seen = true;
	// jfont started for another language (chosen again since it
	// started): English, not the wrong language, until a restart.
	for (int i = 0; i < (int)sizeof(d->lang); i++) {
		if (d->lang[i] != tr_lang[i]) return 0;
		if (!tr_lang[i]) break;
	}
	for (int i = 0; i < Z_LANG_ENTRIES; i++) {
		if (d->entry[i].kind != Z_LANG_CATALOG || !d->entry[i].addr) continue;
		const uint8_t *c = (const uint8_t *)(uintptr_t)d->entry[i].addr;
		uint32_t l = d->entry[i].len;
		if (dom_row(c, l, dom, &nd)) { *len = l; return c; }
	}
#endif
	return 0;
}

// The catalogs, if they can be had now. Once jfont is found, what it
// has is all there is: an app with no catalog of its own is English,
// but for the shared library's strings.
static bool ready(void) {
	if (tr_state == TR_READY) return true;
	if (tr_state != TR_WAIT) return false;
	bool seen = false;
	tr_cat = with_domain(tr_dom_app, &tr_len, &seen);
	if (!seen) return false;
	tr_cat_c = with_domain(tr_dom_common, &tr_len_c, &seen);
	const uint8_t *c = tr_cat ? tr_cat : tr_cat_c;
	tr_rule = c ? c[6] : 0;
	tr_state = TR_READY;
	return true;
}

static const char *find(uint32_t hash) {
	const char *t = z_tr_find(tr_cat, tr_len, tr_dom_app, hash);
	if (!t && tr_dom_common != tr_dom_app)
		t = z_tr_find(tr_cat_c, tr_len_c, tr_dom_common, hash);
	return t;
}

// True if t can be drawn as `fit` asks (Z_TR_*).
static bool fits(const char *t, int fit) {
	if (fit == Z_TR_SMALL) return z_tr_fits_small(t);
	if (fit == Z_TR_L9) {
		while (*t) {
			uint32_t cp = z_utf8_next_z(&t);
			if (cp >= 0x80 && !z_cp_to_l9(cp)) return false;
		}
	}
	return true;
}

const char *z_tr_site(const char *ctx, const char *s, const char **cache, int fit) {

	if (tr_state <= TR_EN) { *cache = s; return s; }
	if (!ready()) return s;			// not yet: English, and ask again

	uint32_t h = ctx ? z_tr_hash2(ctx, 4, s) : z_tr_hash(s);
	const char *t = find(h);
	if (!t || (fit && !fits(t, fit))) t = s;
	*cache = t;
	return t;

}

const char *z_tr_at(const char **c, const char *s) {
	return *c ? *c : z_tr_site(0, s, c, Z_TR_ANY);
}

const char *z_tr_at_small(const char **c, const char *s) {
	return *c ? *c : z_tr_site(0, s, c, Z_TR_SMALL);
}

const char *z_tr_at_l9(const char **c, const char *s) {
	return *c ? *c : z_tr_site(0, s, c, Z_TR_L9);
}

const char *z_tr_at_c(const char **c, const char *ctx, const char *s) {
	return *c ? *c : z_tr_site(ctx, s, c, Z_TR_ANY);
}

const char *z_tr(const char *s) {
	if (!s || tr_state <= TR_EN || !ready()) return s;
	const char *t = find(z_tr_hash(s));
	return t ? t : s;
}

const char *z_tr_c(const char *ctx, const char *s) {
	if (!s || tr_state <= TR_EN || !ready()) return s;
	const char *t = find(z_tr_hash2(ctx, 4, s));
	return t ? t : s;
}

const char *z_tr_small(const char *s) {
	const char *t = z_tr(s);
	return t == s || z_tr_fits_small(t) ? t : s;
}

const char *z_tr_fit(const char *s, int fit) {
	const char *t = z_tr(s);
	return t == s || fits(t, fit) ? t : s;
}

const char *z_tr_cfit(const char *ctx, const char *s, int fit) {
	const char *t = z_tr_c(ctx, s);
	return t == s || fits(t, fit) ? t : s;
}

const char *z_tr_n(const char *one, const char *many, long n) {
	const char *en = n == 1 ? one : many;
	if (tr_state <= TR_EN || !ready()) return en;
	const char *t = find(z_tr_hash2(one, 0, many));
	if (!t) return en;
	int forms = (unsigned char)t[0];
	int k = z_tr_plural(tr_rule, n);
	if (forms <= 0) return en;
	if (k >= forms) k = forms - 1;
	t++;
	while (k--) t += strlen(t) + 1;
	return t;
}

const char *z_tr_nfit(const char *one, const char *many, long n, int fit) {
	const char *t = z_tr_n(one, many, n);
	const char *en = n == 1 ? one : many;
	return t == en || fits(t, fit) ? t : en;
}

// -- what draws small --

bool z_tr_fits_small(const char *s) {

	uint32_t len = 0;
	const uint8_t *f = 0;
	bool looked = false;

	while (*s) {
		uint32_t cp = z_utf8_next_z(&s);
		if (cp < 0x80 || z_cp_to_l9(cp)) continue;	// ASCII, Latin-9: in hardware
		if (!looked) { f = z_lang_data(Z_LANG_FONT_5X8, &len); looked = true; }
		if (!f || !z_zfn_glyph(f, len, cp, 0, 0)) return false;
	}
	return true;

}

#endif
