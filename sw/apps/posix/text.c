/*
 * posix -- sed, tr, cut, tee, cmp, xxd and date (docs/posix.md, "Text
 * tools").
 *
 * Builtins like sh.c's filters: each reads its files, or the previous
 * stage of a pipeline (sh->in, or `< file`), and writes where the
 * command's output goes. No port, message or window code, so they run
 * in the host tests (tests/). A command runs on an 8 KB coroutine stack
 * (job.c), so anything larger than a few hundred bytes is static --
 * which is also why only one command runs at a time.
 *
 * Long ones check px_intr() as they go: Ctrl+C stops them at the next
 * line or chunk.
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "zeitlos.h"
#include "zfsapp.h"
#include "../../common/zrtc.h"
#include "posix.h"
#include "rx.h"

// -- reading: a file, or what came before --

// Calls fn for each chunk of `path`'s bytes, or of sh->in when path is
// NULL. False if the file cannot be opened.
typedef void (*chunk_fn)(px_shell_t *sh, const char *p, int n, void *ctx);

static bool feed_bytes(px_shell_t *sh, const char *path, chunk_fn fn, void *ctx) {
	static char buf[1024];
	if (!path) {
		if (sh->in && sh->in_len) fn(sh, sh->in, sh->in_len, ctx);
		return true;
	}
	char abs[PX_PATH_MAX];
	if (!px_resolve(path, abs, sizeof(abs))) return false;
	int h = fs_open_read(abs);
	if (h < 0) return false;
	for (;;) {
		int got = fs_read_chunk(h, buf, (int)sizeof(buf));
		if (got <= 0 || px_intr(sh)) break;
		fn(sh, buf, got, ctx);
	}
	fs_close_handle(h);
	return true;
}

static bool is_digit(char c) { return c >= '0' && c <= '9'; }

// ================================================================
// sed
// ================================================================
//
// A stream editor for the everyday cases: substitute, delete, print,
// quit, line numbers -- each under an address: a line number, `$`, a
// /regex/, or a range of two, `!` for every other line.
//
//   sed 's/old/new/g' f        sed -n '/TODO/p' f       sed '1,3d' f
//   sed -E 's/(\w+) (\w+)/\2 \1/'                      sed -i 's/a/b/' f
//
// Each line goes through every command in order, then is printed unless
// -n or `d`. Expressions are rx.c's: basic by default, extended with -E
// (or -r). `-i` writes FILE.part and puts it in place of FILE once all
// of it is written: a stopped or failed edit leaves FILE as it was.

#define SED_CMDS    8           // commands in a script
#define SED_RX      10          // regular expressions in it: s and /addresses/, ~1.8 KB each
#define SED_REPL    256

typedef struct {
	int kind;                   // 0 none, 'n' a line, '$', '/' a regex
	long line;
	int rx;                     // the regex, an index into sed_rx
} sed_addr_t;

typedef struct {
	sed_addr_t a1, a2;          // a2.kind 0: one address
	bool neg;                   // `!`
	bool in_range;
	char cmd;                   // s d p q =
	int rx;                     // s: its regex
	char repl[SED_REPL];
	int repl_len;
	bool global, print;
	int nth;                    // s///N: only the Nth match
} sed_cmd_t;

static rx_t sed_rx[SED_RX];
static int sed_nrx;
static sed_cmd_t sed_cmds[SED_CMDS];
static int sed_ncmds;
static bool sed_quiet, sed_ext, sed_quit;
static bool sed_no_newline;         // the input's last line ended without one
static long sed_lineno;
static char sed_err[96];

// Output: the command's own, or the file `-i` is writing.
static int sed_out_h = -1;
static bool sed_out_failed;

static void sed_emit(px_shell_t *sh, const char *p, int n) {
	if (sed_out_h >= 0) {
		if (fs_write_chunk(sed_out_h, p, n) != n) sed_out_failed = true;
	} else px_write(sh, p, n);
}

// A delimited part of a command: up to an unescaped `delim`. The
// escape is kept for the regex (it means something there) and in the
// replacement, except before the delimiter itself.
static const char *sed_part(const char *p, char delim, char *out, int cap, int *len) {
	int n = 0;
	while (*p && *p != delim) {
		if (n + 2 >= cap) return NULL;
		if (*p == '\\' && p[1]) {
			if (p[1] == delim) out[n++] = delim;            // \/ is the delimiter itself
			else { out[n++] = '\\'; out[n++] = p[1]; }     // kept: \( \1 \n mean something
			p += 2;
			continue;
		}
		out[n++] = *p++;
	}
	if (*p != delim) return NULL;
	out[n] = 0;
	*len = n;
	return p + 1;
}

static bool sed_regex(const char *pat, int n, int *idx) {
	if (sed_nrx >= SED_RX) { snprintf(sed_err, sizeof(sed_err), "too many regular expressions"); return false; }
	char e[80];
	if (!n) { snprintf(sed_err, sizeof(sed_err), "an empty regular expression"); return false; }
	if (!rx_compile(&sed_rx[sed_nrx], pat, n, sed_ext, e, sizeof(e))) {
		snprintf(sed_err, sizeof(sed_err), "%s", e);
		return false;
	}
	*idx = sed_nrx++;
	return true;
}

static const char *sed_address(const char *p, sed_addr_t *a) {
	static char pat[PX_LINE_MAX];
	int len;
	a->kind = 0;
	if (is_digit(*p)) {
		a->kind = 'n';
		a->line = 0;
		while (is_digit(*p)) a->line = a->line * 10 + (*p++ - '0');
	} else if (*p == '$') {
		a->kind = '$';
		p++;
	} else if (*p == '/') {
		p = sed_part(p + 1, '/', pat, sizeof(pat), &len);
		if (!p) { snprintf(sed_err, sizeof(sed_err), "unterminated address regex"); return NULL; }
		if (!sed_regex(pat, len, &a->rx)) return NULL;
		a->kind = '/';
	}
	return p;
}

// Parses the whole script: commands separated by ';' or newlines.
static bool sed_parse(const char *p) {
	static char pat[PX_LINE_MAX];
	while (*p) {
		while (*p == ' ' || *p == '\t' || *p == ';' || *p == '\n') p++;
		if (!*p) break;
		if (sed_ncmds >= SED_CMDS) { snprintf(sed_err, sizeof(sed_err), "more than %d commands", SED_CMDS); return false; }
		sed_cmd_t *c = &sed_cmds[sed_ncmds];
		memset(c, 0, sizeof(*c));
		if (!(p = sed_address(p, &c->a1))) return false;
		if (c->a1.kind && *p == ',') {
			if (!(p = sed_address(p + 1, &c->a2))) return false;
			if (!c->a2.kind) { snprintf(sed_err, sizeof(sed_err), "a range needs a second address"); return false; }
		}
		while (*p == ' ') p++;
		if (*p == '!') { c->neg = true; p++; while (*p == ' ') p++; }
		c->cmd = *p;
		if (!*p) { snprintf(sed_err, sizeof(sed_err), "missing command"); return false; }
		p++;
		switch (c->cmd) {
		case 'd': case 'p': case 'q': case '=':
			break;
		case 's': {
			char delim = *p;
			int len;
			if (!delim || delim == '\\' || delim == '\n') { snprintf(sed_err, sizeof(sed_err), "s: bad delimiter"); return false; }
			p = sed_part(p + 1, delim, pat, sizeof(pat), &len);
			if (!p) { snprintf(sed_err, sizeof(sed_err), "unterminated `s' command"); return false; }
			if (!sed_regex(pat, len, &c->rx)) return false;
			p = sed_part(p, delim, c->repl, sizeof(c->repl), &c->repl_len);
			if (!p) { snprintf(sed_err, sizeof(sed_err), "unterminated `s' command"); return false; }
			for (;;) {
				if (*p == 'g') c->global = true;
				else if (*p == 'p') c->print = true;
				else if (is_digit(*p)) { c->nth = 0; while (is_digit(*p)) c->nth = c->nth * 10 + (*p++ - '0'); continue; }
				else break;
				p++;
			}
			break;
		}
		default:
			snprintf(sed_err, sizeof(sed_err), "unknown command: `%c'", c->cmd);
			return false;
		}
		while (*p == ' ' || *p == '\t') p++;
		if (*p && *p != ';' && *p != '\n') { snprintf(sed_err, sizeof(sed_err), "extra characters after command"); return false; }
		sed_ncmds++;
	}
	return true;
}

static bool sed_match_addr(const sed_addr_t *a, const char *line, int len, bool last) {
	int cap[2 * RX_GROUPS];
	bool tl;
	switch (a->kind) {
	case 'n': return sed_lineno == a->line;
	case '$': return last;
	case '/': return rx_exec(&sed_rx[a->rx], line, len, 0, cap, &tl);
	}
	return true;
}

static bool sed_selects(sed_cmd_t *c, const char *line, int len, bool last) {
	bool hit;
	if (!c->a1.kind) hit = true;
	else if (!c->a2.kind) hit = sed_match_addr(&c->a1, line, len, last);
	else if (c->in_range) {
		hit = true;
		// the range ends on a line matching its second address -- or,
		// for a number, on that line or as soon as it is past it
		if (c->a2.kind == 'n' ? sed_lineno >= c->a2.line : sed_match_addr(&c->a2, line, len, last))
			c->in_range = false;
	} else if (sed_match_addr(&c->a1, line, len, last)) {
		hit = true;
		c->in_range = !(c->a2.kind == 'n' && c->a2.line <= sed_lineno);
	} else hit = false;
	return hit != c->neg;
}

// The pattern space, and a second buffer to build a substitution in.
static char sed_ps[PX_LINE_MAX * 2], sed_new[PX_LINE_MAX * 2];
static int sed_ps_len;

static bool sed_subst(px_shell_t *sh, sed_cmd_t *c) {
	int cap[2 * RX_GROUPS];
	int at = 0, n = 0, count = 0, last_end = -1;
	bool any = false, tl = false, over = false;
	while (at <= sed_ps_len && rx_exec(&sed_rx[c->rx], sed_ps, sed_ps_len, at, cap, &tl)) {
		// an empty match right where the last one ended is not one
		// (`s/b*/x/g` on abc is xaxcx): the next character goes as it is
		if (cap[0] == cap[1] && cap[0] == last_end) {
			if (cap[0] >= sed_ps_len) break;
			if (n + 1 > (int)sizeof(sed_new)) { over = true; break; }
			sed_new[n++] = sed_ps[cap[0]];
			at = cap[0] + 1;
			continue;
		}
		last_end = cap[1];
		count++;
		bool take = c->nth ? count == c->nth : true;
		// what came before the match
		int pre = cap[0] - at;
		if (n + pre > (int)sizeof(sed_new)) { over = true; break; }
		memcpy(sed_new + n, sed_ps + at, (size_t)pre);
		n += pre;
		if (take) {
			for (int i = 0; i < c->repl_len; i++) {
				char ch = c->repl[i];
				const char *src = &ch;
				int sl = 1;
				if (ch == '&') { src = sed_ps + cap[0]; sl = cap[1] - cap[0]; }
				else if (ch == '\\' && i + 1 < c->repl_len) {
					char e = c->repl[++i];
					if (e >= '0' && e <= '9') {
						int g = e - '0';
						src = sed_ps + cap[2 * g];
						sl = cap[2 * g] < 0 ? 0 : cap[2 * g + 1] - cap[2 * g];
					} else if (e == 'n') { ch = '\n'; }
					else if (e == 't') { ch = '\t'; }
					else ch = e;
				}
				if (n + sl > (int)sizeof(sed_new)) { over = true; break; }
				memcpy(sed_new + n, src, (size_t)sl);
				n += sl;
			}
			any = true;
		} else {
			int ml = cap[1] - cap[0];
			if (n + ml > (int)sizeof(sed_new)) { over = true; break; }
			memcpy(sed_new + n, sed_ps + cap[0], (size_t)ml);
			n += ml;
		}
		if (over) break;
		// an empty match moves on one character, copying it
		if (cap[1] == cap[0]) {
			if (cap[1] < sed_ps_len) { if (n + 1 > (int)sizeof(sed_new)) { over = true; break; } sed_new[n++] = sed_ps[cap[1]]; }
			at = cap[1] + 1;
		} else at = cap[1];
		if ((!c->global && take) || at > sed_ps_len) break;
	}
	if (tl) { px_puts(sh, "sed: a regular expression took too long on a line\n"); sh->status = 1; }
	if (over) { px_puts(sh, "sed: a line grew past what sed holds -- left as it was\n"); sh->status = 1; return false; }
	if (!any) return false;
	if (at < sed_ps_len) {
		if (n + sed_ps_len - at > (int)sizeof(sed_new)) { px_puts(sh, "sed: a line grew past what sed holds\n"); sh->status = 1; return false; }
		memcpy(sed_new + n, sed_ps + at, (size_t)(sed_ps_len - at));
		n += sed_ps_len - at;
	}
	memcpy(sed_ps, sed_new, (size_t)n);
	sed_ps_len = n;
	return true;
}

// A line out. The input's last line, if it had no newline, is written
// without one -- and should anything follow it, the newline goes first
// (GNU sed's rule).
static bool sed_nl_owed;

static void sed_put_line(px_shell_t *sh, const char *p, int n, bool last) {
	if (sed_nl_owed) { sed_emit(sh, "\n", 1); sed_nl_owed = false; }
	sed_emit(sh, p, n);
	if (last && sed_no_newline) sed_nl_owed = true;
	else sed_emit(sh, "\n", 1);
}

// One line through the script.
static void sed_cycle(px_shell_t *sh, const char *line, int len, bool last) {
	char num[24];
	sed_lineno++;
	memcpy(sed_ps, line, (size_t)len);
	sed_ps_len = len;
	for (int i = 0; i < sed_ncmds; i++) {
		sed_cmd_t *c = &sed_cmds[i];
		if (!sed_selects(c, sed_ps, sed_ps_len, last)) continue;
		switch (c->cmd) {
		case 'd': return;
		case 'p': sed_put_line(sh, sed_ps, sed_ps_len, last); break;
		case '=': snprintf(num, sizeof(num), "%ld", sed_lineno); sed_put_line(sh, num, (int)strlen(num), false); break;
		case 'q': sed_quit = true; goto out;
		case 's':
			if (sed_subst(sh, c) && c->print) sed_put_line(sh, sed_ps, sed_ps_len, last);
			break;
		}
	}
out:
	if (!sed_quiet) sed_put_line(sh, sed_ps, sed_ps_len, last);
}

// Lines are held back one, so the last is known for `$`.
static char sed_held[PX_LINE_MAX];
static int sed_held_len;
static bool sed_have_held, sed_cut;
static char sed_line[PX_LINE_MAX];
static int sed_line_len;

static void sed_line_done(px_shell_t *sh) {
	if (sed_have_held && !sed_quit) sed_cycle(sh, sed_held, sed_held_len, false);
	memcpy(sed_held, sed_line, (size_t)sed_line_len);
	sed_held_len = sed_line_len;
	sed_have_held = true;
	sed_line_len = 0;
}

static void sed_bytes(px_shell_t *sh, const char *p, int n, void *ctx) {
	(void)ctx;
	for (int i = 0; i < n && !sed_quit; i++) {
		if (p[i] == '\n') { sed_line_done(sh); continue; }
		if (sed_line_len < PX_LINE_MAX - 1) sed_line[sed_line_len++] = p[i];
		else sed_cut = true;
	}
}

void px_bi_sed(px_shell_t *sh, int argc, char **argv) {
	static char script[PX_LINE_MAX];
	const char *file = NULL;
	bool in_place = false, have_script = false;
	int sl = 0;

	sed_nrx = sed_ncmds = 0;
	sed_quiet = sed_ext = sed_quit = sed_cut = sed_no_newline = sed_nl_owed = false;
	sed_lineno = 0;
	sed_err[0] = 0;
	script[0] = 0;

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (a[0] == '-' && a[1] && !have_script && strcmp(a, "-e")) {
			for (const char *o = a + 1; *o; o++) {
				if (*o == 'n') sed_quiet = true;
				else if (*o == 'E' || *o == 'r') sed_ext = true;
				else if (*o == 'i') in_place = true;
				else { px_printf(sh, "sed: unknown option -%c\n", *o); sh->status = 1; return; }
			}
			continue;
		}
		if (!strcmp(a, "-e")) {
			if (++i >= argc) { px_puts(sh, "sed: -e needs a script\n"); sh->status = 1; return; }
			int l = (int)strlen(argv[i]);
			if (sl + l + 2 > (int)sizeof(script)) { px_puts(sh, "sed: script too long\n"); sh->status = 1; return; }
			if (sl) script[sl++] = '\n';
			memcpy(script + sl, argv[i], (size_t)l + 1);
			sl += l;
			have_script = true;
			continue;
		}
		if (!have_script) {
			snprintf(script, sizeof(script), "%s", a);
			sl = (int)strlen(script);
			have_script = true;
			continue;
		}
		if (file) { px_puts(sh, "sed: one file at a time\n"); sh->status = 1; return; }
		file = a;
	}
	if (!have_script) {
		px_puts(sh, "usage: sed [-n] [-E] [-i] SCRIPT [FILE]   (-e SCRIPT, more than once)\n");
		sh->status = 1;
		return;
	}
	if (!sed_parse(script)) { px_printf(sh, "sed: %s\n", sed_err); sh->status = 1; return; }
	if (in_place && !file) { px_puts(sh, "sed: -i needs a file\n"); sh->status = 1; return; }

	char abs[PX_PATH_MAX], part[PX_PATH_MAX + 8];
	if (in_place) {
		if (!px_resolve(file, abs, sizeof(abs))) { px_printf(sh, "sed: %s: bad path\n", file); sh->status = 1; return; }
		snprintf(part, sizeof(part), "%s.part", abs);
		sed_out_h = fs_open_write(part);
		sed_out_failed = false;
		if (sed_out_h < 0) { px_printf(sh, "sed: %s: cannot write\n", part); sed_out_h = -1; sh->status = 1; return; }
	}

	sed_have_held = false;
	sed_line_len = 0;
	bool ok = feed_bytes(sh, file, sed_bytes, NULL);
	if (ok && !sed_quit && !px_intr(sh)) {
		if (sed_line_len) { sed_line_done(sh); sed_no_newline = true; }   // a last line with no newline
		if (sed_have_held && !sed_quit) sed_cycle(sh, sed_held, sed_held_len, true);
	}
	if (!ok) px_printf(sh, "sed: %s: cannot open\n", file);
	if (sed_cut) px_printf(sh, "sed: a line was longer than %d and was cut\n", PX_LINE_MAX - 1);

	if (in_place) {
		int h = sed_out_h;
		sed_out_h = -1;
		bool closed = fs_close_handle(h) != 0;
		if (!ok || sed_out_failed || !closed || px_intr(sh) || sed_cut) {
			fs_unlink(part);
			px_printf(sh, "sed: %s left as it was\n", file);
			sh->status = 1;
			return;
		}
		// FAT renames onto nothing: the old file goes first
		if (!fs_unlink(abs) || !fs_rename(part, abs, NULL)) {
			px_printf(sh, "sed: %s: could not replace it -- the edit is in %s\n", file, part);
			sh->status = 1;
			return;
		}
	}
	if (!ok) sh->status = 1;
}

// ================================================================
// tr
// ================================================================
//
//   tr a-z A-Z      tr -d '\r'      tr -s ' '      tr -cd '[:alnum:]\n'
//
// Reads what came before it in a pipeline, or `< file`, as tr does.
// Sets take ranges, \n \t \\ \NNN (octal) and [:class:].

static int tr_expand(const char *s, uint8_t *out) {
	int n = 0;
	while (*s && n < 256) {
		int c;
		if (s[0] == '[' && s[1] == ':') {
			const char *e = strstr(s, ":]");
			if (e) {
				int len = (int)(e - s - 2);
				const char *nm = s + 2;
				for (int k = 0; k < 256 && n < 256; k++) {
#define C(w) (len == (int)sizeof(w) - 1 && !memcmp(nm, w, (size_t)len))
					bool in = C("alpha") ? ((k|32) >= 'a' && (k|32) <= 'z') :
						C("digit") ? (k >= '0' && k <= '9') :
						C("alnum") ? (((k|32) >= 'a' && (k|32) <= 'z') || (k >= '0' && k <= '9')) :
						C("upper") ? (k >= 'A' && k <= 'Z') :
						C("lower") ? (k >= 'a' && k <= 'z') :
						C("space") ? (k == ' ' || (k >= 9 && k <= 13)) :
						C("blank") ? (k == ' ' || k == '\t') :
						C("punct") ? (k > 32 && k < 127 && !(((k|32) >= 'a' && (k|32) <= 'z') || (k >= '0' && k <= '9'))) :
						C("cntrl") ? (k < 32 || k == 127) :
						C("print") ? (k >= 32 && k < 127) : false;
#undef C
					if (in) out[n++] = (uint8_t)k;
				}
				s = e + 2;
				continue;
			}
		}
		if (*s == '\\' && s[1]) {
			s++;
			if (*s >= '0' && *s <= '7') {
				c = 0;
				for (int d = 0; d < 3 && *s >= '0' && *s <= '7'; d++) c = c * 8 + (*s++ - '0');
			} else {
				c = *s == 'n' ? '\n' : *s == 't' ? '\t' : *s == 'r' ? '\r' : (uint8_t)*s;
				s++;
			}
		} else c = (uint8_t)*s++;
		if (*s == '-' && s[1]) {
			int hi = (uint8_t)s[1];
			s += 2;
			for (int k = c; k <= hi && n < 256; k++) out[n++] = (uint8_t)k;
			continue;
		}
		out[n++] = (uint8_t)c;
	}
	return n;
}

static struct {
	int16_t map[256];           // -1: delete; else the byte it becomes
	bool squeeze[256];
	int last;                   // the last byte written, for -s
} tr;

static void tr_bytes(px_shell_t *sh, const char *p, int n, void *ctx) {
	static char out[1024];
	int k = 0;
	(void)ctx;
	for (int i = 0; i < n; i++) {
		int m = tr.map[(uint8_t)p[i]];
		if (m < 0) continue;
		if (tr.squeeze[m] && tr.last == m) continue;
		tr.last = m;
		out[k++] = (char)m;
		if (k == (int)sizeof(out)) { px_write(sh, out, k); k = 0; }
	}
	if (k) px_write(sh, out, k);
}

void px_bi_tr(px_shell_t *sh, int argc, char **argv) {
	static uint8_t s1[256], s2[256];
	bool del = false, sq = false, comp = false;
	int i = 1;
	for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
		for (const char *o = argv[i] + 1; *o; o++) {
			if (*o == 'd') del = true;
			else if (*o == 's') sq = true;
			else if (*o == 'c' || *o == 'C') comp = true;
			else { px_printf(sh, "tr: unknown option -%c\n", *o); sh->status = 1; return; }
		}
	}
	int nargs = argc - i;
	if (nargs < 1 || nargs > 2 || (!del && !sq && nargs != 2) || (del && !sq && nargs != 1)) {
		px_puts(sh, "usage: tr [-c] [-d] [-s] SET1 [SET2]   (reads a pipe, or < FILE)\n");
		sh->status = 1;
		return;
	}
	int n1 = tr_expand(argv[i], s1);
	int n2 = nargs == 2 ? tr_expand(argv[i + 1], s2) : 0;

	bool in1[256];
	memset(in1, 0, sizeof(in1));
	for (int k = 0; k < n1; k++) in1[s1[k]] = true;
	if (comp) for (int k = 0; k < 256; k++) in1[k] = !in1[k];

	for (int k = 0; k < 256; k++) { tr.map[k] = (int16_t)k; tr.squeeze[k] = false; }
	tr.last = -1;
	if (del) {
		for (int k = 0; k < 256; k++) if (in1[k]) tr.map[k] = -1;
		if (sq) for (int k = 0; k < n2; k++) tr.squeeze[s2[k]] = true;
	} else if (nargs == 2) {
		// SET1's bytes become SET2's, the last of SET2 repeated if shorter
		if (!n2) { px_puts(sh, "tr: SET2 is empty\n"); sh->status = 1; return; }
		if (comp) { for (int k = 0; k < 256; k++) if (in1[k]) tr.map[k] = s2[n2 - 1]; }
		else for (int k = 0; k < n1; k++) tr.map[s1[k]] = s2[k < n2 ? k : n2 - 1];
		if (sq) for (int k = 0; k < n2; k++) tr.squeeze[s2[k]] = true;
	} else {
		for (int k = 0; k < 256; k++) if (in1[k]) tr.squeeze[k] = true;
	}
	if (!sh->in) return;            // nothing to read: tr reads only its input
	feed_bytes(sh, NULL, tr_bytes, NULL);
}

// ================================================================
// cut
// ================================================================
//
//   cut -d: -f1,3 f      cut -f2- f (tabs)      cut -c1-8 f      cut -s -d, -f2
//
// Fields or characters (bytes) by a list: N, N-M, N-, -M, comma-separated.

#define CUT_RANGES 16
static struct { int lo, hi; } cut_r[CUT_RANGES];
static int cut_n;
static char cut_delim;
static bool cut_chars, cut_only_delim;

static bool cut_list(const char *s) {
	cut_n = 0;
	while (*s) {
		int lo = 1, hi = 1 << 30;
		if (cut_n >= CUT_RANGES) return false;
		if (is_digit(*s)) { lo = 0; while (is_digit(*s)) lo = lo * 10 + (*s++ - '0'); hi = lo; }
		if (*s == '-') {
			s++;
			hi = 1 << 30;
			if (is_digit(*s)) { hi = 0; while (is_digit(*s)) hi = hi * 10 + (*s++ - '0'); }
		}
		if (lo < 1 || hi < lo) return false;
		cut_r[cut_n].lo = lo;
		cut_r[cut_n].hi = hi;
		cut_n++;
		if (*s == ',') s++;
		else if (*s) return false;
	}
	return cut_n > 0;
}

static bool cut_want(int k) {
	for (int i = 0; i < cut_n; i++) if (k >= cut_r[i].lo && k <= cut_r[i].hi) return true;
	return false;
}

static void cut_line(px_shell_t *sh, const char *line, int len, void *ctx) {
	(void)ctx;
	if (cut_chars) {
		for (int i = 0; i < len; i++) if (cut_want(i + 1)) px_write(sh, line + i, 1);
		px_write(sh, "\n", 1);
		return;
	}
	if (!memchr(line, cut_delim, (size_t)len)) {
		if (!cut_only_delim) { px_write(sh, line, len); px_write(sh, "\n", 1); }
		return;
	}
	int field = 1, start = 0;
	bool first = true;
	for (int i = 0; i <= len; i++) {
		if (i < len && line[i] != cut_delim) continue;
		if (cut_want(field)) {
			if (!first) px_write(sh, &cut_delim, 1);
			px_write(sh, line + start, i - start);
			first = false;
		}
		field++;
		start = i + 1;
	}
	px_write(sh, "\n", 1);
}

void px_bi_cut(px_shell_t *sh, int argc, char **argv) {
	const char *list = NULL, *file = NULL;
	cut_delim = '\t';
	cut_chars = cut_only_delim = false;
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (a[0] == '-' && (a[1] == 'd' || a[1] == 'f' || a[1] == 'c' || a[1] == 'b')) {
			const char *v = a[2] ? a + 2 : (i + 1 < argc ? argv[++i] : NULL);
			if (!v) { px_printf(sh, "cut: -%c needs a value\n", a[1]); sh->status = 1; return; }
			if (a[1] == 'd') {
				if (strlen(v) != 1) { px_puts(sh, "cut: the delimiter must be one character\n"); sh->status = 1; return; }
				cut_delim = v[0];
			} else {
				list = v;
				cut_chars = a[1] != 'f';
			}
		} else if (!strcmp(a, "-s")) cut_only_delim = true;
		else if (!file) file = a;
		else { px_puts(sh, "cut: one file at a time\n"); sh->status = 1; return; }
	}
	if (!list || !cut_list(list)) {
		px_puts(sh, "usage: cut -f LIST [-d C] [-s] [FILE]  or  cut -c LIST [FILE]   (LIST: 1,3-5,7-)\n");
		sh->status = 1;
		return;
	}
	bool cut;
	if (!px_feed_lines(sh, file, cut_line, NULL, &cut)) {
		px_printf(sh, "cut: %s: cannot open\n", file);
		sh->status = 1;
		return;
	}
	if (cut) px_printf(sh, "cut: a line was longer than %d and was cut\n", PX_LINE_MAX - 1);
}

// ================================================================
// tee
// ================================================================
//
//   ... | tee out.txt | wc        ... | tee -a log.txt

#define TEE_FILES 4
static int tee_h[TEE_FILES], tee_n;
static bool tee_failed;

static void tee_bytes(px_shell_t *sh, const char *p, int n, void *ctx) {
	(void)ctx;
	for (int i = 0; i < tee_n; i++)
		if (tee_h[i] >= 0 && fs_write_chunk(tee_h[i], p, n) != n) tee_failed = true;
	px_write(sh, p, n);
}

void px_bi_tee(px_shell_t *sh, int argc, char **argv) {
	bool append = false;
	char err[128];
	tee_n = 0;
	tee_failed = false;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-a")) { append = true; continue; }
		if (tee_n >= TEE_FILES) { px_printf(sh, "tee: at most %d files\n", TEE_FILES); sh->status = 1; return; }
		int fd = px_open_append(argv[i], append, err, sizeof(err));
		if (fd < 0) { px_printf(sh, "tee: %s\n", err); sh->status = 1; continue; }
		tee_h[tee_n++] = px_fd(fd)->handle;
	}
	feed_bytes(sh, NULL, tee_bytes, NULL);
	if (tee_failed) { px_puts(sh, "tee: a write failed\n"); sh->status = 1; }
	// the files are closed after the command, with the rest (sh.c)
}

// ================================================================
// cmp
// ================================================================
//
//   cmp a b         "a b differ: byte 7, line 2", status 1
//   cmp -s a b      the status only

void px_bi_cmp(px_shell_t *sh, int argc, char **argv) {
	static char b1[1024], b2[1024];
	bool silent = false;
	const char *f[2];
	int nf = 0;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-s")) silent = true;
		else if (nf < 2) f[nf++] = argv[i];
		else nf = 3;
	}
	if (nf != 2) { px_puts(sh, "usage: cmp [-s] FILE1 FILE2\n"); sh->status = 2; return; }
	int h[2];
	for (int k = 0; k < 2; k++) {
		char abs[PX_PATH_MAX];
		h[k] = px_resolve(f[k], abs, sizeof(abs)) ? fs_open_read(abs) : -1;
		if (h[k] < 0) {
			px_printf(sh, "cmp: %s: cannot open\n", f[k]);
			if (k) fs_close_handle(h[0]);
			sh->status = 2;
			return;
		}
	}
	long byte = 1, line = 1;
	int n1 = 0, n2 = 0, i1 = 0, i2 = 0;
	sh->status = 0;
	for (;;) {
		if (i1 == n1) { n1 = fs_read_chunk(h[0], b1, sizeof(b1)); i1 = 0; if (n1 < 0) n1 = 0; }
		if (i2 == n2) { n2 = fs_read_chunk(h[1], b2, sizeof(b2)); i2 = 0; if (n2 < 0) n2 = 0; }
		if (!n1 && !n2) break;
		if (!n1 || !n2) {
			if (!silent) px_printf(sh, "cmp: EOF on %s after byte %ld, line %ld\n", f[!n1 ? 0 : 1], byte - 1, line);
			sh->status = 1;
			break;
		}
		if (b1[i1] != b2[i2]) {
			if (!silent) px_printf(sh, "%s %s differ: byte %ld, line %ld\n", f[0], f[1], byte, line);
			sh->status = 1;
			break;
		}
		if (b1[i1] == '\n') line++;
		i1++; i2++; byte++;
		if (!(byte & 4095) && px_intr(sh)) break;
	}
	fs_close_handle(h[0]);
	fs_close_handle(h[1]);
}

// ================================================================
// xxd
// ================================================================
//
//   xxd f        00000000: 6865 6c6c 6f0a                           hello.
//   xxd -s 16 -l 32 f

static struct { uint32_t at, skip, left; bool limit; uint8_t row[16]; int n; } xx;

static void xxd_row(px_shell_t *sh) {
	char line[80];
	int k = snprintf(line, sizeof(line), "%08lx: ", (unsigned long)xx.at);
	for (int i = 0; i < 16; i++) {
		if (i < xx.n) k += snprintf(line + k, sizeof(line) - (size_t)k, "%02x", xx.row[i]);
		else { line[k++] = ' '; line[k++] = ' '; }
		if (i & 1) line[k++] = ' ';
	}
	line[k++] = ' ';
	for (int i = 0; i < xx.n; i++) line[k++] = (xx.row[i] >= 32 && xx.row[i] < 127) ? (char)xx.row[i] : '.';
	line[k++] = '\n';
	px_write(sh, line, k);
	xx.at += (uint32_t)xx.n;
	xx.n = 0;
}

static void xxd_bytes(px_shell_t *sh, const char *p, int n, void *ctx) {
	(void)ctx;
	for (int i = 0; i < n; i++) {
		if (xx.skip) { xx.skip--; continue; }
		if (xx.limit && !xx.left) return;
		if (xx.limit) xx.left--;
		xx.row[xx.n++] = (uint8_t)p[i];
		if (xx.n == 16) xxd_row(sh);
	}
}

static bool num_arg(const char *s, uint32_t *v) {
	char *e;
	unsigned long x = strtoul(s, &e, 0);         // 0x... too
	if (!*s || *e) return false;
	*v = (uint32_t)x;
	return true;
}

void px_bi_xxd(px_shell_t *sh, int argc, char **argv) {
	const char *file = NULL;
	memset(&xx, 0, sizeof(xx));
	for (int i = 1; i < argc; i++) {
		if ((!strcmp(argv[i], "-s") || !strcmp(argv[i], "-l")) && i + 1 < argc) {
			uint32_t v;
			if (!num_arg(argv[i + 1], &v)) { px_printf(sh, "xxd: %s: not a number\n", argv[i + 1]); sh->status = 1; return; }
			if (argv[i][1] == 's') xx.skip = v;
			else { xx.left = v; xx.limit = true; }
			i++;
		} else if (!file && argv[i][0] != '-') file = argv[i];
		else { px_puts(sh, "usage: xxd [-s OFFSET] [-l LENGTH] [FILE]\n"); sh->status = 1; return; }
	}
	xx.at = xx.skip;
	if (!feed_bytes(sh, file, xxd_bytes, NULL)) {
		px_printf(sh, "xxd: %s: cannot open\n", file);
		sh->status = 1;
		return;
	}
	if (xx.n) xxd_row(sh);
}

// ================================================================
// date
// ================================================================
//
//   date            Sat Nov  7 14:05:09 CET 2026   (system.rtc.timezone)
//   date -u         ... UTC
//   date +%Y-%m-%d  2026-11-07

void px_bi_date(px_shell_t *sh, int argc, char **argv) {
	static char out[PX_LINE_MAX];
	const char *fmt = "%a %b %e %H:%M:%S %Z %Y";
	bool utc_only = false;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-u")) utc_only = true;
		else if (argv[i][0] == '+') fmt = argv[i] + 1;
		else { px_puts(sh, "usage: date [-u] [+FORMAT]   (%Y %m %d %e %H %M %S %a %b %j %s %Z %z %F %T %%)\n"); sh->status = 1; return; }
	}
	uint32_t utc;
	if (!px_clock_now(&utc)) {
		px_puts(sh, "date: the clock is not set (it is set from the network, by NTP)\n");
		sh->status = 1;
		return;
	}
	z_tz_t tz;
	char zone[64];
	bool dst = false;
	if (utc_only || !px_clock_zone(zone, sizeof(zone))) z_tz_parse("UTC", &tz);
	else if (!z_tz_parse(zone, &tz)) px_printf(sh, "date: system.rtc.timezone '%s' is not understood -- UTC\n", zone);
	uint32_t local = z_tz_local(&tz, utc, &dst);
	int32_t off = (int32_t)(local - utc);
	z_tm_t tm;
	z_time_to_tm(local, &tm);

	int k = 0;
	unsigned Y = tm.year, mo = tm.month, d = tm.day, H = tm.hour, Mi = tm.min, S = tm.sec;
	for (const char *p = fmt; *p && k < (int)sizeof(out) - 2; p++) {
		char f[40];
		f[0] = 0;
		if (*p != '%' || !p[1]) { out[k++] = *p; continue; }
		switch (*++p) {
		case 'Y': snprintf(f, sizeof(f), "%04u", Y); break;
		case 'm': snprintf(f, sizeof(f), "%02u", mo); break;
		case 'd': snprintf(f, sizeof(f), "%02u", d); break;
		case 'e': snprintf(f, sizeof(f), "%2u", d); break;
		case 'H': snprintf(f, sizeof(f), "%02u", H); break;
		case 'M': snprintf(f, sizeof(f), "%02u", Mi); break;
		case 'S': snprintf(f, sizeof(f), "%02u", S); break;
		case 'a': snprintf(f, sizeof(f), "%s", z_wday_name(tm.wday)); break;
		case 'b': snprintf(f, sizeof(f), "%s", z_month_name(tm.month)); break;
		case 'j': snprintf(f, sizeof(f), "%03u", (unsigned)tm.yday + 1); break;
		case 's': snprintf(f, sizeof(f), "%lu", (unsigned long)utc); break;
		case 'Z': snprintf(f, sizeof(f), "%s", z_tz_name(&tz, dst)); break;
		case 'z': snprintf(f, sizeof(f), "%c%02ld%02ld", off < 0 ? '-' : '+',
				(long)(labs((long)off) / 3600), (long)(labs((long)off) / 60 % 60)); break;
		case 'F': snprintf(f, sizeof(f), "%04u-%02u-%02u", Y, mo, d); break;
		case 'T': snprintf(f, sizeof(f), "%02u:%02u:%02u", H, Mi, S); break;
		case 'n': snprintf(f, sizeof(f), "\n"); break;
		case '%': snprintf(f, sizeof(f), "%%"); break;
		default: snprintf(f, sizeof(f), "%%%c", *p); break;
		}
		for (const char *q = f; *q && k < (int)sizeof(out) - 2; q++) out[k++] = *q;
	}
	out[k++] = '\n';
	px_write(sh, out, k);
}
