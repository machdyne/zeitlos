/*
 * slide -- reading and writing .zsl. See deckio.h.
 *
 * The reader is a cursor over the text: `(`, `)`, atoms (symbols and
 * integers) and strings. Forms it does not know are skipped whole, so a
 * deck written by a later slide still opens here (its new parts are
 * dropped, and say so in nothing worse than their absence).
 */

#include "deckio.h"
#include "../../common/zpool.h"

#include <string.h>

// -- the buffer ----------------------------------------------------------

void sb_init(sbuf_t *b) { memset(b, 0, sizeof(*b)); }

void sb_free(sbuf_t *b) { pool_free(b->buf); sb_init(b); }

void sb_put(sbuf_t *b, const char *s, uint32_t n) {
	if (b->oom) return;
	if (b->len + n + 1 > b->cap) {
		uint32_t nc = b->cap ? b->cap : 1024;
		char *nb;
		while (nc < b->len + n + 1) nc *= 2;
		nb = pool_realloc(b->buf, nc);
		if (!nb) { b->oom = true; return; }
		b->buf = nb;
		b->cap = nc;
	}
	memcpy(b->buf + b->len, s, n);
	b->len += n;
	b->buf[b->len] = 0;
}

void sb_puts(sbuf_t *b, const char *s) { sb_put(b, s, (uint32_t)strlen(s)); }

void sb_int(sbuf_t *b, long v) {
	char t[24];
	int k = 0;
	unsigned long u = v < 0 ? (unsigned long)-v : (unsigned long)v;
	do { t[k++] = (char)('0' + u % 10); u /= 10; } while (u);
	if (v < 0) t[k++] = '-';
	while (k) sb_put(b, &t[--k], 1);
}

void sb_str(sbuf_t *b, const char *s) {
	sb_put(b, "\"", 1);
	if (s) {
		for (; *s; s++) {
			unsigned char c = (unsigned char)*s;
			if (c == '"') sb_put(b, "\\\"", 2);
			else if (c == '\\') sb_put(b, "\\\\", 2);
			else if (c == '\n') sb_put(b, "\\n", 2);
			else if (c == '\t') sb_put(b, "\\t", 2);
			else if (c < 0x20) continue;
			else sb_put(b, (const char *)&c, 1);
		}
	}
	sb_put(b, "\"", 1);
}

// -- writing -------------------------------------------------------------

static const char *tr_names[TR_KINDS] = { "cut", "wipe", "push", "iris", "dissolve" };
static const char *al_names[3] = { "left", "center", "right" };
static const char *va_names[3] = { "top", "middle", "bottom" };

static void ind(sbuf_t *b, int n) {
	static const char sp[] = "\t\t\t\t\t\t";
	sb_put(b, "\n", 1);
	sb_put(b, sp, (uint32_t)(n < 6 ? n : 6));
}

static void form_int(sbuf_t *b, const char *head, long v) {
	sb_puts(b, " (");
	sb_puts(b, head);
	sb_puts(b, " ");
	sb_int(b, v);
	sb_puts(b, ")");
}

static void form_str(sbuf_t *b, const char *head, const char *s) {
	sb_puts(b, " (");
	sb_puts(b, head);
	sb_puts(b, " ");
	sb_str(b, s);
	sb_puts(b, ")");
}

static void write_obj(sbuf_t *b, const deck_t *d, const obj_t *o, int depth) {
	ind(b, depth);
	sb_puts(b, "(");
	sb_puts(b, obj_kind_name(o->kind));
	sb_puts(b, " ");
	sb_str(b, o->name);
	sb_puts(b, " (box ");
	sb_int(b, o->x); sb_puts(b, " ");
	sb_int(b, o->y); sb_puts(b, " ");
	sb_int(b, o->w); sb_puts(b, " ");
	sb_int(b, o->h); sb_puts(b, ")");
	if (o->fill == FILL_SHADE) form_int(b, "fill shade", o->fill_v);
	else if (o->fill == FILL_PATTERN) form_int(b, "fill pattern", o->fill_v);
	else sb_puts(b, " (fill none)");
	sb_puts(b, " (line ");
	sb_int(b, o->line_w);
	sb_puts(b, " ");
	sb_int(b, o->line_tone);
	sb_puts(b, ")");
	if (o->kind == OB_RRECT || o->kind == OB_BUTTON) form_int(b, "radius", o->radius);
	if (o->kind == OB_LINE || o->kind == OB_POLY) {
		static const char *ar[4] = { "none", "start", "end", "both" };
		sb_puts(b, " (arrows ");
		sb_puts(b, ar[o->arrows & 3]);
		sb_puts(b, ")");
	}
	if (o->kind == OB_POLY) {
		int i;
		if (o->closed) sb_puts(b, " (closed)");
		if (o->smooth) sb_puts(b, " (smooth)");
		sb_puts(b, " (points");
		for (i = 0; i < o->npts; i++) {
			sb_puts(b, " ");
			sb_int(b, o->pts[2 * i]);
			sb_puts(b, " ");
			sb_int(b, o->pts[2 * i + 1]);
		}
		sb_puts(b, ")");
	}
	if (o->kind == OB_TEXT || OB_IS_CONTROL(o->kind) || (o->text && o->text[0])) {
		sb_puts(b, " (font ");
		sb_str(b, (o->font < d->nfont) ? d->font[o->font] : "sans");
		sb_puts(b, " ");
		sb_int(b, o->size);
		if (o->style & ST_BOLD) sb_puts(b, " bold");
		if (o->style & ST_ITALIC) sb_puts(b, " italic");
		if (o->style & ST_UNDERLINE) sb_puts(b, " underline");
		sb_puts(b, ") (align ");
		sb_puts(b, al_names[o->align < 3 ? o->align : 0]);
		sb_puts(b, " ");
		sb_puts(b, va_names[o->valign < 3 ? o->valign : 0]);
		sb_puts(b, ")");
		form_int(b, "color", o->text_tone);
		form_int(b, "pad", o->pad);
		form_str(b, "text", o->text ? o->text : "");
	}
	if (o->kind == OB_IMAGE) {
		form_str(b, "src", o->src ? o->src : "");
		if (o->contrast) form_int(b, "contrast", o->contrast);
		if (o->bright) form_int(b, "bright", o->bright);
	}
	if (o->kind == OB_CHECK || o->kind == OB_RADIO || o->kind == OB_SLIDER || o->kind == OB_LIST)
		form_int(b, "value", o->value);
	if (o->kind == OB_SLIDER) {
		sb_puts(b, " (range ");
		sb_int(b, o->vmin);
		sb_puts(b, " ");
		sb_int(b, o->vmax);
		sb_puts(b, ")");
	}
	if (o->kind == OB_RADIO && o->group[0]) form_str(b, "group", o->group);
	if (obj_is_connector(o)) {
		sb_puts(b, " (connect ");
		sb_str(b, o->from);
		sb_puts(b, " ");
		sb_str(b, o->to);
		sb_puts(b, ")");
	}
	if (o->gid) form_int(b, "gid", o->gid);
	if (o->flags & OBF_FLIPX) sb_puts(b, " (flip across)");
	if (o->flags & OBF_FLIPY) sb_puts(b, " (flip down)");
	if (o->flags & OBF_HIDDEN) sb_puts(b, " (hidden)");
	if (o->flags & OBF_LOCKED) sb_puts(b, " (locked)");
	if (o->action) form_str(b, "action", o->action);
	if (o->script) form_str(b, "script", o->script);
	sb_puts(b, ")");
}

static void write_card(sbuf_t *b, const deck_t *d, const card_t *c, bool is_bg,
	int depth) {
	int i;
	ind(b, depth);
	sb_puts(b, is_bg ? "(background " : "(card ");
	sb_str(b, c->name);
	if (!is_bg && c->bg >= 0 && c->bg < d->nbg) form_str(b, "background", d->bg[c->bg]->name);
	if (!is_bg && (c->trans || c->trans_ms != 400)) {
		sb_puts(b, " (transition ");
		sb_puts(b, tr_names[c->trans < TR_KINDS ? c->trans : 0]);
		sb_puts(b, " ");
		sb_int(b, c->trans_ms);
		sb_puts(b, ")");
	}
	if (c->advance_ms) form_int(b, "advance", (long)c->advance_ms);
	if (c->action) form_str(b, "action", c->action);
	for (i = 0; i < c->nobj; i++) write_obj(b, d, c->obj[i], depth + 1);
	if (c->notes) { ind(b, depth + 1); sb_puts(b, "(notes "); sb_str(b, c->notes); sb_puts(b, ")"); }
	if (c->script) { ind(b, depth + 1); sb_puts(b, "(script "); sb_str(b, c->script); sb_puts(b, ")"); }
	sb_puts(b, ")");
}

bool deckio_write(sbuf_t *b, const deck_t *d) {
	int i;
	sb_puts(b, "; slide deck (docs/slide_app.md)\n(deck (version 1)");
	ind(b, 1);
	sb_puts(b, "(page ");
	if (!strcmp(d->page.size.name, "custom")) {
		sb_puts(b, "custom ");
		sb_puts(b, d->page.size.kind == ZPAGE_PAPER ? "paper " : "ratio ");
		sb_int(b, d->page.size.w);
		sb_puts(b, " ");
		sb_int(b, d->page.size.h);
	} else {
		sb_str(b, d->page.size.name);
	}
	sb_puts(b, d->page.landscape ? " landscape)" : " portrait)");
	if (d->title[0]) { ind(b, 1); sb_puts(b, "(title "); sb_str(b, d->title); sb_puts(b, ")"); }
	if (d->print[0]) { ind(b, 1); sb_puts(b, "(print "); sb_str(b, d->print); sb_puts(b, ")"); }
	if (d->app) { ind(b, 1); sb_puts(b, "(app)"); }
	if (d->loop) { ind(b, 1); sb_puts(b, "(loop)"); }
	if (d->script) { ind(b, 1); sb_puts(b, "(script "); sb_str(b, d->script); sb_puts(b, ")"); }
	for (i = 0; i < d->nbg; i++) write_card(b, d, d->bg[i], true, 1);
	for (i = 0; i < d->ncard; i++) write_card(b, d, d->card[i], false, 1);
	sb_puts(b, ")\n");
	return !b->oom;
}

bool deckio_write_card(sbuf_t *b, const deck_t *d, const card_t *c, bool is_bg) {
	write_card(b, d, c, is_bg, 0);
	sb_puts(b, "\n");
	return !b->oom;
}

bool deckio_write_objs(sbuf_t *b, const deck_t *d, obj_t *const *o, int n) {
	int i;
	sb_puts(b, "(objects");
	for (i = 0; i < n; i++) write_obj(b, d, o[i], 1);
	sb_puts(b, ")\n");
	return !b->oom;
}

// -- reading -------------------------------------------------------------

enum { T_EOF = 0, T_OPEN, T_CLOSE, T_ATOM, T_STR, T_BAD };

#define MAX_DEPTH 24
#define ATOM_MAX 48

typedef struct {
	const char	*s, *e;
	int			line;
	int			tok;
	char		atom[ATOM_MAX];
	const char	*str;			// T_STR: raw, between the quotes
	uint32_t	strlen;
	bool		failed;
	char		*err;
	int			errcap;
	deck_t		*d;
} rd_t;

static void fail(rd_t *r, const char *why) {
	if (r->failed) return;
	r->failed = true;
	if (r->err && r->errcap > 0) {
		sbuf_t t;
		uint32_t n;
		sb_init(&t);
		sb_puts(&t, "line ");
		sb_int(&t, r->line);
		sb_puts(&t, ": ");
		sb_puts(&t, why);
		n = t.buf ? t.len : 0;
		if (n >= (uint32_t)r->errcap) n = (uint32_t)r->errcap - 1;
		if (t.buf) memcpy(r->err, t.buf, n);
		r->err[n] = 0;
		sb_free(&t);
	}
}

static void next(rd_t *r) {
	for (;;) {
		while (r->s < r->e && (*r->s == ' ' || *r->s == '\t' || *r->s == '\r' || *r->s == '\n')) {
			if (*r->s == '\n') r->line++;
			r->s++;
		}
		if (r->s < r->e && *r->s == ';') {
			while (r->s < r->e && *r->s != '\n') r->s++;
			continue;
		}
		break;
	}
	if (r->failed || r->s >= r->e) { r->tok = T_EOF; return; }
	if (*r->s == '(') { r->s++; r->tok = T_OPEN; return; }
	if (*r->s == ')') { r->s++; r->tok = T_CLOSE; return; }
	if (*r->s == '"') {
		const char *p = ++r->s;
		while (r->s < r->e && *r->s != '"') {
			if (*r->s == '\\' && r->s + 1 < r->e) r->s++;
			if (*r->s == '\n') r->line++;
			r->s++;
		}
		if (r->s >= r->e) { fail(r, "a string never ends"); r->tok = T_BAD; return; }
		r->str = p;
		r->strlen = (uint32_t)(r->s - p);
		r->s++;
		r->tok = T_STR;
		return;
	}
	{
		int n = 0;
		while (r->s < r->e && *r->s != ' ' && *r->s != '\t' && *r->s != '\n' &&
			*r->s != '\r' && *r->s != '(' && *r->s != ')' && *r->s != '"' && *r->s != ';') {
			if (n < ATOM_MAX - 1) r->atom[n++] = *r->s;
			r->s++;
		}
		r->atom[n] = 0;
		r->tok = T_ATOM;
	}
}

// The current string, unescaped, into the pool.
static char *take_str(rd_t *r) {
	char *o = pool_alloc(r->strlen + 1);
	uint32_t i, k = 0;
	if (!o) { fail(r, "out of memory"); return NULL; }
	for (i = 0; i < r->strlen; i++) {
		char c = r->str[i];
		if (c == '\\' && i + 1 < r->strlen) {
			c = r->str[++i];
			if (c == 'n') c = '\n';
			else if (c == 't') c = '\t';
		}
		o[k++] = c;
	}
	o[k] = 0;
	return o;
}

static void str_into(rd_t *r, char *dst, int cap) {
	uint32_t i;
	int k = 0;
	for (i = 0; i < r->strlen && k < cap - 1; i++) {
		char c = r->str[i];
		if (c == '\\' && i + 1 < r->strlen) c = r->str[++i];
		if ((unsigned char)c < 0x20) continue;
		dst[k++] = c;
	}
	// never cut a UTF-8 sequence in half
	while (k > 0 && ((unsigned char)dst[k - 1] & 0xC0) == 0x80) k--;
	if (k > 0 && ((unsigned char)dst[k - 1] & 0xC0) == 0xC0) k--;
	dst[k] = 0;
	if (i < r->strlen) {
		// truncated: k is a whole-character prefix already
	}
}

static bool is_int(const char *a) {
	int i = (a[0] == '-') ? 1 : 0;
	if (!a[i]) return false;
	for (; a[i]; i++) if (a[i] < '0' || a[i] > '9') return false;
	return true;
}

static long to_int(const char *a) {
	long v = 0;
	int i = (a[0] == '-') ? 1 : 0;
	for (; a[i] && v < 100000000L; i++) v = v * 10 + (a[i] - '0');
	return a[0] == '-' ? -v : v;
}

// An integer atom, clamped, or fail.
static long rd_int(rd_t *r, long lo, long hi) {
	long v;
	next(r);
	if (r->tok != T_ATOM || !is_int(r->atom)) { fail(r, "a number was expected"); return lo; }
	v = to_int(r->atom);
	if (v < lo) v = lo;
	if (v > hi) v = hi;
	return v;
}

// Skips to the close of the current form (the open already read).
static void skip_rest(rd_t *r) {
	int depth = 1;
	while (depth > 0 && !r->failed) {
		next(r);
		if (r->tok == T_OPEN) { if (++depth > MAX_DEPTH) fail(r, "nested too deep"); }
		else if (r->tok == T_CLOSE) depth--;
		else if (r->tok == T_EOF) fail(r, "a form never closes");
	}
}

// Reads the closing paren of a form whose contents are done.
static void close_form(rd_t *r) {
	next(r);
	if (r->tok != T_CLOSE) {
		// extra items: tolerated (a later version's), skipped
		if (r->tok == T_EOF) { fail(r, "a form never closes"); return; }
		if (r->tok == T_OPEN) skip_rest(r);
		skip_rest(r);
	}
}

static bool rd_str(rd_t *r) {
	next(r);
	if (r->tok != T_STR) { fail(r, "a string was expected"); return false; }
	return true;
}

static void set_str(rd_t *r, char **field) {
	char *t;
	if (!rd_str(r)) return;
	t = take_str(r);
	if (!t) return;
	pool_free(*field);
	*field = t;
	close_form(r);
}

static obj_t *read_obj(rd_t *r, int kind) {
	obj_t *o = obj_new(kind);
	if (!o) { fail(r, "out of memory"); return NULL; }
	if (!rd_str(r)) { obj_free(o); return NULL; }
	str_into(r, o->name, OB_NAME_MAX);
	for (;;) {
		next(r);
		if (r->failed) break;
		if (r->tok == T_CLOSE) return o;
		if (r->tok != T_OPEN) { fail(r, "a property was expected"); break; }
		next(r);
		if (r->tok != T_ATOM) { skip_rest(r); continue; }
		if (!strcmp(r->atom, "box")) {
			o->x = (int16_t)rd_int(r, -9999, 9999);
			o->y = (int16_t)rd_int(r, -9999, 9999);
			o->w = (int16_t)rd_int(r, -9999, 9999);
			o->h = (int16_t)rd_int(r, -9999, 9999);
			close_form(r);
		} else if (!strcmp(r->atom, "fill")) {
			next(r);
			if (r->tok == T_ATOM && !strcmp(r->atom, "shade")) {
				o->fill = FILL_SHADE;
				o->fill_v = (uint8_t)rd_int(r, 0, 16);
			} else if (r->tok == T_ATOM && !strcmp(r->atom, "pattern")) {
				o->fill = FILL_PATTERN;
				o->fill_v = (uint8_t)rd_int(r, 0, 15);
			} else {
				o->fill = FILL_NONE;
			}
			close_form(r);
		} else if (!strcmp(r->atom, "line")) {
			o->line_w = (uint8_t)rd_int(r, 0, 64);
			next(r);
			if (r->tok == T_ATOM && is_int(r->atom)) {
				long t = to_int(r->atom);
				o->line_tone = (uint8_t)(t < 0 ? 0 : t > 16 ? 16 : t);
				close_form(r);
			} else if (r->tok != T_CLOSE) {
				fail(r, "line: width and tone");
			}
		} else if (!strcmp(r->atom, "radius")) {
			o->radius = (uint8_t)rd_int(r, 0, 255);
			close_form(r);
		} else if (!strcmp(r->atom, "arrows")) {
			next(r);
			if (r->tok == T_ATOM) {
				if (!strcmp(r->atom, "start")) o->arrows = AR_START;
				else if (!strcmp(r->atom, "end")) o->arrows = AR_END;
				else if (!strcmp(r->atom, "both")) o->arrows = AR_START | AR_END;
				else o->arrows = 0;
			}
			close_form(r);
		} else if (!strcmp(r->atom, "closed")) {
			o->closed = 1;
			close_form(r);
		} else if (!strcmp(r->atom, "smooth")) {
			o->smooth = 1;
			close_form(r);
		} else if (!strcmp(r->atom, "points")) {
			static int16_t tmp[2 * 1024];
			int n = 0;
			for (;;) {
				next(r);
				if (r->tok == T_CLOSE) break;
				if (r->tok != T_ATOM || !is_int(r->atom)) { fail(r, "points: numbers"); break; }
				if (n < 2 * 1024) {
					long v = to_int(r->atom);
					tmp[n++] = (int16_t)(v < -9999 ? -9999 : v > 9999 ? 9999 : v);
				}
			}
			n &= ~1;
			pool_free(o->pts);
			o->pts = NULL;
			o->npts = 0;
			if (n) {
				o->pts = pool_alloc((size_t)n * 2);
				if (!o->pts) { fail(r, "out of memory"); break; }
				memcpy(o->pts, tmp, (size_t)n * 2);
				o->npts = (uint16_t)(n / 2);
			}
		} else if (!strcmp(r->atom, "font")) {
			if (!rd_str(r)) break;
			{
				char nm[32];
				str_into(r, nm, sizeof(nm));
				o->font = (uint8_t)deck_font_index(r->d, nm[0] ? nm : "sans");
			}
			o->size = (uint8_t)rd_int(r, 4, 255);
			o->style = 0;
			for (;;) {
				next(r);
				if (r->tok == T_CLOSE) break;
				if (r->tok != T_ATOM) { fail(r, "font: style"); break; }
				if (!strcmp(r->atom, "bold")) o->style |= ST_BOLD;
				else if (!strcmp(r->atom, "italic")) o->style |= ST_ITALIC;
				else if (!strcmp(r->atom, "underline")) o->style |= ST_UNDERLINE;
			}
		} else if (!strcmp(r->atom, "align")) {
			for (;;) {
				int i;
				next(r);
				if (r->tok == T_CLOSE) break;
				if (r->tok != T_ATOM) { fail(r, "align"); break; }
				for (i = 0; i < 3; i++) {
					if (!strcmp(r->atom, al_names[i])) o->align = (uint8_t)i;
					if (!strcmp(r->atom, va_names[i])) o->valign = (uint8_t)i;
				}
			}
		} else if (!strcmp(r->atom, "color")) {
			o->text_tone = (uint8_t)rd_int(r, 0, 16);
			close_form(r);
		} else if (!strcmp(r->atom, "pad")) {
			o->pad = (uint8_t)rd_int(r, 0, 100);
			close_form(r);
		} else if (!strcmp(r->atom, "text")) {
			set_str(r, &o->text);
		} else if (!strcmp(r->atom, "src")) {
			set_str(r, &o->src);
		} else if (!strcmp(r->atom, "contrast")) {
			o->contrast = (int8_t)rd_int(r, -100, 100);
			close_form(r);
		} else if (!strcmp(r->atom, "bright")) {
			o->bright = (int8_t)rd_int(r, -100, 100);
			close_form(r);
		} else if (!strcmp(r->atom, "hidden")) {
			o->flags |= OBF_HIDDEN;
			close_form(r);
		} else if (!strcmp(r->atom, "locked")) {
			o->flags |= OBF_LOCKED;
			close_form(r);
		} else if (!strcmp(r->atom, "value")) {
			o->value = (int16_t)rd_int(r, -9999, 9999);
			close_form(r);
		} else if (!strcmp(r->atom, "range")) {
			o->vmin = (int16_t)rd_int(r, -9999, 9999);
			o->vmax = (int16_t)rd_int(r, -9999, 9999);
			if (o->vmax <= o->vmin) o->vmax = (int16_t)(o->vmin + 1);
			close_form(r);
		} else if (!strcmp(r->atom, "gid")) {
			o->gid = (uint16_t)rd_int(r, 0, 65535);
			close_form(r);
		} else if (!strcmp(r->atom, "flip")) {
			next(r);
			if (r->tok == T_ATOM && !strcmp(r->atom, "across")) o->flags |= OBF_FLIPX;
			if (r->tok == T_ATOM && !strcmp(r->atom, "down")) o->flags |= OBF_FLIPY;
			if (r->tok != T_CLOSE) close_form(r);
		} else if (!strcmp(r->atom, "connect")) {
			if (!rd_str(r)) break;
			str_into(r, o->from, OB_NAME_MAX);
			if (!rd_str(r)) break;
			str_into(r, o->to, OB_NAME_MAX);
			close_form(r);
		} else if (!strcmp(r->atom, "group")) {
			if (!rd_str(r)) break;
			str_into(r, o->group, sizeof(o->group));
			close_form(r);
		} else if (!strcmp(r->atom, "action")) {
			set_str(r, &o->action);
		} else if (!strcmp(r->atom, "script")) {
			set_str(r, &o->script);
		} else {
			skip_rest(r);
		}
	}
	obj_free(o);
	return NULL;
}

// After `(card` or `(background`: the name, then the card's forms.
static card_t *read_card(rd_t *r, bool is_bg) {
	card_t *c;
	if (!rd_str(r)) return NULL;
	{
		char nm[CARD_NAME_MAX];
		str_into(r, nm, sizeof(nm));
		c = card_new(nm);
	}
	if (!c) { fail(r, "out of memory"); return NULL; }
	for (;;) {
		int kind;
		next(r);
		if (r->failed) break;
		if (r->tok == T_CLOSE) return c;
		if (r->tok != T_OPEN) { fail(r, "a card holds forms"); break; }
		next(r);
		if (r->tok != T_ATOM) { skip_rest(r); continue; }
		kind = obj_kind_of(r->atom);
		if (kind >= 0) {
			obj_t *o = read_obj(r, kind);
			if (!o) break;
			if (!card_add(c, -1, o)) { obj_free(o); fail(r, "out of memory"); break; }
		} else if (!strcmp(r->atom, "background") && !is_bg) {
			char nm[CARD_NAME_MAX];
			if (!rd_str(r)) break;
			str_into(r, nm, sizeof(nm));
			c->bg = deck_find_bg(r->d, nm);
			close_form(r);
		} else if (!strcmp(r->atom, "transition")) {
			int i;
			next(r);
			if (r->tok == T_ATOM)
				for (i = 0; i < TR_KINDS; i++) if (!strcmp(r->atom, tr_names[i])) c->trans = (uint8_t)i;
			c->trans_ms = (uint16_t)rd_int(r, 0, 10000);
			close_form(r);
		} else if (!strcmp(r->atom, "advance")) {
			c->advance_ms = (uint32_t)rd_int(r, 0, 3600000);
			close_form(r);
		} else if (!strcmp(r->atom, "notes")) {
			set_str(r, &c->notes);
		} else if (!strcmp(r->atom, "action")) {
			set_str(r, &c->action);
		} else if (!strcmp(r->atom, "script")) {
			set_str(r, &c->script);
		} else {
			skip_rest(r);
		}
	}
	card_free(c);
	return NULL;
}

static void read_page(rd_t *r, deck_t *d) {
	bool land = true;
	next(r);
	if (r->tok == T_STR) {
		char nm[16];
		str_into(r, nm, sizeof(nm));
		next(r);
		if (r->tok == T_ATOM) land = strcmp(r->atom, "portrait") != 0;
		if (!zpage_set(&d->page, nm, land)) zpage_set(&d->page, "classic", true);
		if (r->tok != T_CLOSE) close_form(r);
		return;
	}
	if (r->tok == T_ATOM && !strcmp(r->atom, "custom")) {
		int kind;
		long w, h;
		next(r);
		kind = (r->tok == T_ATOM && !strcmp(r->atom, "ratio")) ? ZPAGE_RATIO : ZPAGE_PAPER;
		w = rd_int(r, 1, 60000);
		h = rd_int(r, 1, 60000);
		next(r);
		if (r->tok == T_ATOM) land = strcmp(r->atom, "portrait") != 0;
		if (!zpage_custom(&d->page, kind, (int)w, (int)h, land)) zpage_set(&d->page, "classic", true);
		if (r->tok != T_CLOSE) close_form(r);
		return;
	}
	fail(r, "page: a size");
}

deck_t *deckio_read(const char *s, uint32_t len, char *err, int errcap) {
	rd_t r;
	deck_t *d = pool_zalloc(sizeof(deck_t));
	memset(&r, 0, sizeof(r));
	r.s = s;
	r.e = s + len;
	r.line = 1;
	r.err = err;
	r.errcap = errcap;
	if (err && errcap) err[0] = 0;
	if (!d) { fail(&r, "out of memory"); return NULL; }
	r.d = d;
	zpage_set(&d->page, "classic", true);
	deck_font_index(d, "sans");

	next(&r);
	if (r.tok != T_OPEN) { fail(&r, "not a slide deck"); goto bad; }
	next(&r);
	if (r.tok != T_ATOM || strcmp(r.atom, "deck")) { fail(&r, "not a slide deck"); goto bad; }
	for (;;) {
		next(&r);
		if (r.failed) goto bad;
		if (r.tok == T_CLOSE) break;
		if (r.tok != T_OPEN) { fail(&r, "a deck holds forms"); goto bad; }
		next(&r);
		if (r.tok != T_ATOM) { skip_rest(&r); continue; }
		if (!strcmp(r.atom, "version")) {
			long v = rd_int(&r, 0, 1000000);
			if (v > 1) { fail(&r, "made by a newer slide"); goto bad; }
			close_form(&r);
		} else if (!strcmp(r.atom, "page")) {
			read_page(&r, d);
		} else if (!strcmp(r.atom, "title")) {
			if (!rd_str(&r)) goto bad;
			str_into(&r, d->title, DECK_TITLE_MAX);
			close_form(&r);
		} else if (!strcmp(r.atom, "print")) {
			if (!rd_str(&r)) goto bad;
			str_into(&r, d->print, DECK_PRINT_MAX);
			close_form(&r);
		} else if (!strcmp(r.atom, "app")) {
			d->app = true;
			close_form(&r);
		} else if (!strcmp(r.atom, "loop")) {
			d->loop = true;
			close_form(&r);
		} else if (!strcmp(r.atom, "script")) {
			set_str(&r, &d->script);
		} else if (!strcmp(r.atom, "card") || !strcmp(r.atom, "background")) {
			bool bg = r.atom[0] == 'b';
			card_t *c = read_card(&r, bg);
			if (!c) goto bad;
			if (!(bg ? deck_insert_bg(d, -1, c) : deck_insert_card(d, -1, c))) {
				card_free(c);
				fail(&r, "out of memory");
				goto bad;
			}
		} else {
			skip_rest(&r);
		}
		if (r.failed) goto bad;
	}
	if (d->ncard == 0) {
		card_t *c = card_new("card 1");
		if (!c || !deck_insert_card(d, 0, c)) { card_free(c); fail(&r, "out of memory"); goto bad; }
	}
	{
		// connectors as their shapes are now (a file edited by hand)
		int i;
		for (i = 0; i < d->nbg; i++) card_route(d->bg[i], NULL, NULL, 0);
		for (i = 0; i < d->ncard; i++)
			card_route(d->card[i], d->card[i]->bg >= 0 && d->card[i]->bg < d->nbg ? d->bg[d->card[i]->bg] : NULL, NULL, 0);
	}
	return d;
bad:
	if (!r.failed) fail(&r, "malformed");
	deck_free(d);
	return NULL;
}

card_t *deckio_read_card(deck_t *d, const char *s, uint32_t len) {
	rd_t r;
	card_t *c;
	memset(&r, 0, sizeof(r));
	r.s = s;
	r.e = s + len;
	r.line = 1;
	r.d = d;
	next(&r);
	if (r.tok != T_OPEN) return NULL;
	next(&r);
	if (r.tok != T_ATOM || (strcmp(r.atom, "card") && strcmp(r.atom, "background"))) return NULL;
	c = read_card(&r, r.atom[0] == 'b');
	return r.failed ? (card_free(c), (card_t *)NULL) : c;
}

int deckio_read_objs(deck_t *d, card_t *into, const char *s, uint32_t len) {
	rd_t r;
	int n = 0;
	memset(&r, 0, sizeof(r));
	r.s = s;
	r.e = s + len;
	r.line = 1;
	r.d = d;
	next(&r);
	if (r.tok != T_OPEN) return -1;
	next(&r);
	if (r.tok != T_ATOM || strcmp(r.atom, "objects")) return -1;
	for (;;) {
		int kind;
		obj_t *o;
		next(&r);
		if (r.failed || r.tok != T_OPEN) break;
		next(&r);
		if (r.tok != T_ATOM) { skip_rest(&r); continue; }
		kind = obj_kind_of(r.atom);
		if (kind < 0) { skip_rest(&r); continue; }
		o = read_obj(&r, kind);
		if (!o) break;
		if (!card_add(into, -1, o)) { obj_free(o); break; }
		n++;
	}
	return n;
}
