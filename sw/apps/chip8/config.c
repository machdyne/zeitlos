/*
 * chip8 -- per-ROM configuration parser. See config.h.
 */

#include <string.h>

#include "config.h"

/* -- small string helpers -------------------------------------------
 *
 * Written out rather than using strtok/strcasecmp: strtok modifies its
 * input and this parser is handed a buffer it does not own, and
 * strcasecmp is not in every libc this tree can be built against
 * (arch.mk supports both newlib and picolibc). Neither is worth a
 * portability surprise for the four lines they would save.
 */

static char lower(char c) {
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool is_space(char c) {
	return c == ' ' || c == '\t' || c == '\r';
}

/* Case-insensitive compare of `a` against the token of length `len` at
 * `b`. Both ends must match: "shift" must not match "shifty". */
static bool tok_eq(const char *b, int len, const char *a) {
	int i;
	for (i = 0; i < len; i++) {
		if (!a[i]) return false;
		if (lower(b[i]) != lower(a[i])) return false;
	}
	return a[len] == '\0';
}

static int tok_len(const char *p) {
	int n = 0;
	while (p[n] && !is_space(p[n]) && p[n] != '\n') n++;
	return n;
}

static const char *skip_space(const char *p) {
	while (*p && is_space(*p)) p++;
	return p;
}

/* One hex digit, or -1. */
static int hex_digit(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	c = lower(c);
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	return -1;
}

static int parse_uint(const char *p, int len) {
	int v = 0, i;
	if (len <= 0) return -1;
	for (i = 0; i < len; i++) {
		if (p[i] < '0' || p[i] > '9') return -1;
		v = v * 10 + (p[i] - '0');
		if (v > 100000) return -1;
	}
	return v;
}

/* on/off/true/false/yes/no/1/0 -> 1/0, or -1 if it is none of those. */
static int parse_bool(const char *p, int len) {
	if (tok_eq(p, len, "on") || tok_eq(p, len, "true") ||
	    tok_eq(p, len, "yes") || tok_eq(p, len, "1")) return 1;
	if (tok_eq(p, len, "off") || tok_eq(p, len, "false") ||
	    tok_eq(p, len, "no") || tok_eq(p, len, "0")) return 0;
	return -1;
}

/* -- tables --------------------------------------------------------- */

static const char *const quirk_names[C8_Q_COUNT] = {
	"vf_reset", "mem_inc", "display_wait", "clip",
	"shift", "jump", "wide_lores", "collision_rows",
	"scroll_half", "clear_on_mode", "memory"
};

/* Must match c8_palette_t in render.h. Named here rather than shared,
 * because config.c deliberately does not include the renderer -- the
 * parser has no business knowing how a grey is made. */
static const char *const palette_names[3] = { "fill", "index", "solid" };

static const char *const pad_names[C8_PAD_COUNT] = {
	"up", "down", "left", "right",
	"a", "b", "x", "y", "start", "select"
};

/* -- parsing -------------------------------------------------------- */

void c8_config_defaults(c8_config_t *cfg) {
	int i;
	memset(cfg, 0, sizeof(*cfg));
	cfg->profile = -1;
	cfg->speed = 0;
	cfg->palette = -1;
	for (i = 0; i < C8_PAD_COUNT; i++) cfg->pad[i] = 0xFF;
}

/* Apply one `k=v` pair from a `set` line. Returns false on anything
 * unrecognised, which the caller counts. */
static bool apply_set(c8_config_t *cfg, const char *k, int klen,
	const char *v, int vlen) {

	int f, b;

	for (f = 0; f < C8_Q_COUNT; f++)
		if (tok_eq(k, klen, quirk_names[f])) break;

	if (f == C8_Q_COUNT) return false;

	switch (f) {

	case C8_Q_MEM_INC:
		if (tok_eq(v, vlen, "x1"))        cfg->quirks.mem_inc = C8_MEM_INC_X1;
		else if (tok_eq(v, vlen, "x"))    cfg->quirks.mem_inc = C8_MEM_INC_X;
		else if (tok_eq(v, vlen, "none")) cfg->quirks.mem_inc = C8_MEM_INC_NONE;
		else return false;
		break;

	case C8_Q_MEMORY:
		if (tok_eq(v, vlen, "4k"))       cfg->quirks.addr_mask = 0x0FFF;
		else if (tok_eq(v, vlen, "64k")) cfg->quirks.addr_mask = 0xFFFF;
		else return false;
		break;

	default:
		b = parse_bool(v, vlen);
		if (b < 0) return false;
		switch (f) {
		case C8_Q_VF_RESET:       cfg->quirks.vf_reset = b; break;
		case C8_Q_DISPLAY_WAIT:   cfg->quirks.display_wait = b; break;
		case C8_Q_CLIP:           cfg->quirks.clip_sprites = b; break;
		case C8_Q_SHIFT:          cfg->quirks.shift_vx = b; break;
		case C8_Q_JUMP:           cfg->quirks.jump_vx = b; break;
		case C8_Q_WIDE_LORES:     cfg->quirks.wide_sprite_lores = b; break;
		case C8_Q_COLLISION_ROWS: cfg->quirks.collision_rows = b; break;
		case C8_Q_SCROLL_HALF:    cfg->quirks.scroll_half_lores = b; break;
		case C8_Q_CLEAR_ON_MODE:  cfg->quirks.clear_on_mode = b; break;
		default: return false;
		}
		break;

	}

	cfg->set_mask |= (uint16_t)(1u << f);
	return true;

}

static bool apply_pad(c8_config_t *cfg, const char *k, int klen,
	const char *v, int vlen) {

	int b, d;

	for (b = 0; b < C8_PAD_COUNT; b++)
		if (tok_eq(k, klen, pad_names[b])) break;

	if (b == C8_PAD_COUNT) return false;
	if (vlen != 1) return false;

	/* A single hex digit, because that is what a guest key IS. "10"
	 * would be ambiguous between decimal ten and hex one-zero, and the
	 * guest keypad has no key 10. */
	d = hex_digit(v[0]);
	if (d < 0) return false;

	cfg->pad[b] = (uint8_t)d;
	return true;

}

/* Split "k=v" and hand it to `fn`. */
static bool apply_pair(c8_config_t *cfg, const char *p, int len,
	bool (*fn)(c8_config_t *, const char *, int, const char *, int)) {

	int i;
	for (i = 0; i < len; i++) if (p[i] == '=') break;
	if (i == 0 || i >= len - 1) return false;
	return fn(cfg, p, i, p + i + 1, len - i - 1);

}

/* Does this section header name `rom`, or is it the `[*]` wildcard? */
static int section_match(const char *p, const char *rom) {

	const char *end;
	int len, i;

	p = skip_space(p);
	if (*p != '[') return 0;
	p++;

	for (end = p; *end && *end != ']' && *end != '\n'; end++)
		;
	if (*end != ']') return 0;

	len = (int)(end - p);
	while (len > 0 && is_space(p[len - 1])) len--;

	if (len == 1 && p[0] == '*') return 2;      /* wildcard */

	for (i = 0; i < len; i++)
		if (lower(p[i]) != lower(rom[i])) return 0;

	return rom[len] == '\0' ? 1 : 0;

}

static void parse_line(c8_config_t *cfg, const char *p, int lineno) {

	int klen;
	const char *rest;

	p = skip_space(p);
	if (!*p || *p == '\n' || *p == '#' || *p == ';') return;

	klen = tok_len(p);
	rest = skip_space(p + klen);

	if (tok_eq(p, klen, "name")) {
		int n = 0;
		while (rest[n] && rest[n] != '\n' && rest[n] != '#' &&
		       n < C8_CFG_NAME_MAX - 1) n++;
		while (n > 0 && is_space(rest[n - 1])) n--;
		memcpy(cfg->name, rest, (size_t)n);
		cfg->name[n] = '\0';
		return;
	}

	if (tok_eq(p, klen, "profile")) {
		int prof = c8_profile_by_name_n(rest, tok_len(rest));
		if (prof < 0) goto bad;
		cfg->profile = prof;
		return;
	}

	if (tok_eq(p, klen, "palette")) {
		int n = tok_len(rest), i;
		for (i = 0; i < 3; i++)
			if (tok_eq(rest, n, palette_names[i])) { cfg->palette = i; return; }
		goto bad;
	}

	if (tok_eq(p, klen, "speed")) {
		int v = parse_uint(rest, tok_len(rest));
		if (v <= 0) goto bad;
		cfg->speed = v;
		return;
	}

	if (tok_eq(p, klen, "set") || tok_eq(p, klen, "pad")) {
		bool is_set = tok_eq(p, klen, "set");
		bool ok = true;
		const char *q = rest;
		while (*q && *q != '\n' && *q != '#' && *q != ';') {
			int n = tok_len(q);
			if (n == 0) break;
			if (!apply_pair(cfg, q, n, is_set ? apply_set : apply_pad))
				ok = false;
			q = skip_space(q + n);
		}
		if (!ok) goto bad;
		return;
	}

bad:
	cfg->bad_lines++;
	if (cfg->first_bad_line == 0) cfg->first_bad_line = lineno;

}

/* One pass over the text, applying only lines inside sections whose
 * match value is `want`. Called twice -- wildcard then named -- so the
 * named section always wins regardless of which appears first in the
 * file. */
static bool pass(c8_config_t *cfg, const char *text, const char *rom,
	int want) {

	const char *p = text;
	bool in = false, matched = false;
	int lineno = 0;

	while (*p) {

		const char *eol = p;
		while (*eol && *eol != '\n') eol++;
		lineno++;

		{
			const char *s = skip_space(p);
			if (*s == '[') {
				int m = section_match(s, rom);
				in = (m == want);
				if (in) matched = true;
			} else if (in) {
				parse_line(cfg, p, lineno);
			}
		}

		p = *eol ? eol + 1 : eol;

	}

	return matched;

}

bool c8_config_parse(c8_config_t *cfg, const char *text, const char *rom) {

	bool w, n;

	if (!text || !rom) return false;

	w = pass(cfg, text, rom, 2);
	n = pass(cfg, text, rom, 1);

	cfg->found = w || n;
	return cfg->found;

}

/* Case-insensitive suffix test. */
static bool ends_with(const char *s, const char *suffix) {

	int sl = 0, fl = 0, i;

	while (s[sl]) sl++;
	while (suffix[fl]) fl++;
	if (fl > sl) return false;

	for (i = 0; i < fl; i++)
		if (lower(s[sl - fl + i]) != lower(suffix[i])) return false;

	return true;

}

int c8_profile_hint(const char *name, uint32_t len) {

	/* Arithmetic beats naming. Nothing this large has anywhere to
	 * live under a 12-bit address space. */
	if (len > C8_ROM_MAX_4K) return C8_PROFILE_XOCHIP;

	if (!name) return C8_PROFILE_CHIP8;

	if (ends_with(name, ".xo8") || ends_with(name, ".xo"))
		return C8_PROFILE_XOCHIP;
	if (ends_with(name, ".sc8") || ends_with(name, ".sc"))
		return C8_PROFILE_SCHIP;

	return C8_PROFILE_CHIP8;

}

void c8_config_apply(const c8_config_t *cfg, c8_quirks_t *base) {

	if (cfg->set_mask & (1u << C8_Q_VF_RESET))
		base->vf_reset = cfg->quirks.vf_reset;
	if (cfg->set_mask & (1u << C8_Q_MEM_INC))
		base->mem_inc = cfg->quirks.mem_inc;
	if (cfg->set_mask & (1u << C8_Q_DISPLAY_WAIT))
		base->display_wait = cfg->quirks.display_wait;
	if (cfg->set_mask & (1u << C8_Q_CLIP))
		base->clip_sprites = cfg->quirks.clip_sprites;
	if (cfg->set_mask & (1u << C8_Q_SHIFT))
		base->shift_vx = cfg->quirks.shift_vx;
	if (cfg->set_mask & (1u << C8_Q_JUMP))
		base->jump_vx = cfg->quirks.jump_vx;
	if (cfg->set_mask & (1u << C8_Q_WIDE_LORES))
		base->wide_sprite_lores = cfg->quirks.wide_sprite_lores;
	if (cfg->set_mask & (1u << C8_Q_COLLISION_ROWS))
		base->collision_rows = cfg->quirks.collision_rows;
	if (cfg->set_mask & (1u << C8_Q_SCROLL_HALF))
		base->scroll_half_lores = cfg->quirks.scroll_half_lores;
	if (cfg->set_mask & (1u << C8_Q_CLEAR_ON_MODE))
		base->clear_on_mode = cfg->quirks.clear_on_mode;
	if (cfg->set_mask & (1u << C8_Q_MEMORY))
		base->addr_mask = cfg->quirks.addr_mask;

}
