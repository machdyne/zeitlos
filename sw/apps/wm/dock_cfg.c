/*
 * Zeitlos -- what the dock shows. See dock_cfg.h and docs/dock.md.
 */

#include <string.h>
#include "dock_cfg.h"

// -- dock.zic --------------------------------------------------------

static uint32_t get32(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
		((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t get16(const uint8_t *p) {
	return (uint16_t)(p[0] | (p[1] << 8));
}

bool dk_zic_header(const uint8_t raw[DK_ZIC_HEADER], uint32_t size, dk_zic_t *z) {

	if (!raw || memcmp(raw, DK_ZIC_MAGIC, 4) != 0) return false;
	z->count        = get16(raw + 4);
	z->spoken_bytes = get32(raw + 8);
	z->icon_bytes   = get32(raw + 12);
	z->names_used   = 0;

	// The entry layout is this file's, exactly: an older or newer one
	// is refused rather than read at the wrong offsets.
	if (get16(raw + 6) != DK_ZIC_ENTRY) return false;
	if (z->count <= 0 || z->count > DK_ZIC_ENTRIES) return false;
	if (z->spoken_bytes > 0xffff || z->icon_bytes > 0xffff) return false;
	uint32_t total = DK_ZIC_HEADER + (uint32_t)z->count * DK_ZIC_ENTRY +
		z->spoken_bytes + z->icon_bytes;
	if (total > DK_ZIC_MAX) return false;
	if (size && size != total) return false;
	return true;

}

bool dk_zic_entry(const uint8_t raw[DK_ZIC_ENTRY], dk_zic_t *z, int i, dk_has_fn has) {

	dk_zic_entry_t *e = &z->e[i];
	int n = 0;

	while (n < DK_NAME_MAX && raw[n]) n++;
	if (n == 0 || n == DK_NAME_MAX) return false;		// empty, or no end
	if (z->names_used + n + 1 > DK_ZIC_NAMES) return false;
	e->name = z->names_used;
	memcpy(z->names + z->names_used, raw, (size_t)n);
	z->names[z->names_used + n] = 0;
	z->names_used = (uint16_t)(z->names_used + n + 1);

	uint32_t f1 = get32(raw + 16), f2 = get32(raw + 20);
	e->flags    = (uint8_t)(get16(raw + 24) & DK_ZIC_HIDDEN);
	if ((f1 || f2) && has && !has(f1, f2)) e->flags |= DK_ZIC_NOHW;
	e->spoken   = get16(raw + 26);
	e->icon     = get16(raw + 28);
	uint16_t len = get16(raw + 30);
	if (e->spoken >= z->spoken_bytes) return false;
	if (len == 0 || len > 255 || (uint32_t)e->icon + len > z->icon_bytes) return false;
	e->icon_len = (uint8_t)len;
	return true;

}

const char *dk_zic_name(const dk_zic_t *z, int i) {
	return z->names + z->e[i].name;
}

int dk_zic_find(const dk_zic_t *z, const char *name) {
	if (!z) return -1;
	for (int i = 0; i < z->count; i++)
		if (strcmp(dk_zic_name(z, i), name) == 0) return i;
	return -1;
}

bool dk_icon_expand(const uint8_t *blob, int len, uint8_t out[DK_ICON_BYTES]) {

	int i = 0, k = 0;

	while (i < len) {
		uint8_t c = blob[i++];
		int n = (c & 0x7f) + 1;
		if (k + n > DK_ICON_BYTES) return false;
		if (c & 0x80) {
			memset(out + k, 0, (size_t)n);
		} else {
			if (i + n > len) return false;
			memcpy(out + k, blob + i, (size_t)n);
			i += n;
		}
		k += n;
	}
	if (k != DK_ICON_BYTES) return false;

	for (k = 4; k < DK_ICON_BYTES; k++) out[k] ^= out[k - 4];	// the row above
	return true;

}

// -- reading dock.zic front to back --------------------------------

// Exactly `n` bytes, through as many reads as it takes.
static bool read_n(dk_read_fn rd, void *ctx, uint8_t *buf, uint32_t n) {
	while (n) {
		int k = rd(ctx, buf, n);
		if (k <= 0 || (uint32_t)k > n) return false;
		buf += k;
		n -= (uint32_t)k;
	}
	return true;
}

int dk_zic_read_table(dk_read_fn rd, void *ctx, dk_zic_t *z, uint8_t *buf, uint32_t buflen,
	dk_has_fn has) {

	if (buflen < DK_ZIC_HEADER || !read_n(rd, ctx, buf, DK_ZIC_HEADER) ||
		!dk_zic_header(buf, 0, z)) return -1;

	uint32_t per = buflen / DK_ZIC_ENTRY;
	if (per == 0) return -1;
	for (int i = 0; i < z->count; ) {
		uint32_t k = (uint32_t)(z->count - i) < per ? (uint32_t)(z->count - i) : per;
		if (!read_n(rd, ctx, buf, k * DK_ZIC_ENTRY)) return -1;
		for (uint32_t j = 0; j < k; j++, i++)
			if (!dk_zic_entry(buf + j * DK_ZIC_ENTRY, z, i, has)) return -1;
	}
	return z->count;

}

// Kept by read_section() while it works: this entry's part was read.
#define GOT_SPOKEN      0x04u
#define GOT_ICON        0x08u

// One section of the file, a chunk at a time through `buf`, keeping the
// bytes the wanted entries ask for -- their spoken names up to the NUL
// (`names`), or their icons -- in the order they come. Each wanted
// entry's offset is changed to where its part went in the store.

static bool read_section(dk_read_fn rd, void *ctx, dk_zic_t *z, bool names, uint32_t size,
	uint8_t *buf, uint32_t buflen, uint8_t *store, uint32_t cap, uint32_t *used) {

	int cur = -1;			// the entry being kept
	uint32_t left = 0;		// of its icon, still to come
	int next = 0;			// where to look for the next wanted one
	uint32_t pos = 0;

	while (pos < size) {

		uint32_t k = size - pos < buflen ? size - pos : buflen;
		if (!read_n(rd, ctx, buf, k)) return false;

		for (uint32_t j = 0; j < k; j++, pos++) {

			if (cur < 0) {
				while (next < z->count && !(z->e[next].flags & DK_ZIC_WANT)) next++;
				if (next >= z->count) continue;		// nothing more wanted: read on
				dk_zic_entry_t *e = &z->e[next];
				uint32_t at = names ? e->spoken : e->icon;
				if (at < pos) return false;		// out of order
				if (at > pos) continue;
				cur = next++;
				if (names) {
					e->flags |= GOT_SPOKEN;
					if (buf[j] == 0) { e->spoken = DK_NONE; cur = -1; continue; }
					e->spoken = (uint16_t)*used;
					left = DK_SPOKEN_MAX;
				} else {
					e->icon = (uint16_t)*used;
					left = e->icon_len;
				}
			}

			if (*used >= cap) {
				if (names) z->e[cur].flags &= (uint8_t)~GOT_SPOKEN;
				return false;
			}
			store[(*used)++] = buf[j];
			if (names ? buf[j] == 0 : --left == 0) {
				if (!names) z->e[cur].flags |= GOT_ICON;
				cur = -1;
			} else if (names && --left == 0) {
				z->e[cur].flags &= (uint8_t)~GOT_SPOKEN;
				return false;				// a name with no end
			}
		}

	}

	return cur < 0;

}

bool dk_zic_read_parts(dk_read_fn rd, void *ctx, dk_zic_t *z, uint8_t *buf, uint32_t buflen,
	uint8_t *store, uint32_t cap, uint32_t *used) {

	bool ok = buflen > 0 &&
		read_section(rd, ctx, z, true, z->spoken_bytes, buf, buflen, store, cap, used) &&
		read_section(rd, ctx, z, false, z->icon_bytes, buf, buflen, store, cap, used);

	// What was not read has none: everything not wanted, and, if this
	// stopped early, whatever was wanted and not reached.
	for (int i = 0; i < z->count; i++) {
		dk_zic_entry_t *e = &z->e[i];
		if (!(e->flags & GOT_SPOKEN)) e->spoken = DK_NONE;
		if (!(e->flags & GOT_ICON)) e->icon_len = 0;
		e->flags &= (uint8_t)~(GOT_SPOKEN | GOT_ICON);
	}
	return ok;

}

// -- wm.cfg ----------------------------------------------------------

void dk_cfg_init(dk_cfg_t *c, dk_report_fn report, void *ctx) {
	memset(c, 0, sizeof(*c));
	c->side = DK_SIDE_BOTTOM;
	c->dots = true;
	for (int i = 0; i < DK_LISTS; i++) c->rest[i] = -1;
	c->cont = -1;
	c->report = report;
	c->ctx = ctx;
}

static void err(dk_cfg_t *c, int line, const char *msg, const char *word) {
	c->errors++;
	if (c->report) c->report(c->ctx, line, msg, word);
}

static bool is_space(char ch) { return ch == ' ' || ch == '\t' || ch == '\r'; }

// The next word of `*s` into `w` (at most `cap` - 1 characters). False
// at the end of the line or at a comment. A word that does not fit is
// returned cut short with *too_long set.
static bool next_word(const char **s, char *w, int cap, bool *too_long) {

	const char *p = *s;
	int n = 0;

	*too_long = false;
	while (is_space(*p)) p++;
	if (!*p || *p == '#') { *s = p; return false; }

	while (*p && !is_space(*p)) {
		if (n < cap - 1) w[n++] = *p;
		else *too_long = true;
		p++;
	}
	w[n] = 0;
	*s = p;
	return true;

}

// A word, or a "quoted name" with spaces in it (a group's name).
static bool next_name(const char **s, char *w, int cap, bool *too_long) {

	const char *p = *s;
	int n = 0;

	while (is_space(*p)) p++;
	if (*p != '"') { *s = p; return next_word(s, w, cap, too_long); }

	*too_long = false;
	p++;
	while (*p && *p != '"') {
		if (n < cap - 1) w[n++] = *p;
		else *too_long = true;
		p++;
	}
	if (*p == '"') p++;
	w[n] = 0;
	*s = p;
	return n > 0;

}

int dk_group_find(const dk_cfg_t *c, const char *id) {
	for (int g = 0; g < c->ngroups; g++)
		if (strcmp(c->groups[g].id, id) == 0) return g;
	return -1;
}

// Adds the rest of `s` to list `list` (a row, or a group).
static void list_words(dk_cfg_t *c, int list, const char *s, int line) {

	char w[DK_NAME_MAX];
	bool too_long;

	while (next_word(&s, w, sizeof(w), &too_long)) {

		if (too_long) { err(c, line, "name too long: ", w); continue; }

		if (strcmp(w, "@rest") == 0) {
			if (c->rest[list] >= 0) { err(c, line, "@rest twice in one list", ""); continue; }
			c->rest[list] = (int16_t)c->count[list];
			continue;
		}

		// A group's icon, kept as "@id" and looked up when the list is
		// made: the group may be defined further down the file.
		if (w[0] == '@' && list >= DK_ROWS) {
			err(c, line, "cannot hold a group: ", w);
			continue;
		}
		if (w[0] == '@' && !w[1]) { err(c, line, "@ with no group name", ""); continue; }

		int n = (int)strlen(w) + 1;
		if (c->used + n > DK_POOL) { err(c, line, "too many names: ", w); continue; }
		memcpy(c->pool + c->used, w, (size_t)n);
		c->used = (uint16_t)(c->used + n);
		c->count[list]++;

	}

}

static void list_begin(dk_cfg_t *c, int list, const char *s, int lineno) {
	c->has[list] = true;
	c->start[list] = c->used;
	c->cont = (int8_t)list;
	list_words(c, list, s, lineno);
}

void dk_cfg_line(dk_cfg_t *c, const char *line, int lineno) {

	char w[DK_SPOKEN_MAX];
	bool too_long;
	const char *s = line;
	bool indented = is_space(line[0]);

	if (!next_word(&s, w, 16, &too_long)) return;	// blank or a comment

	if (indented) {
		if (c->cont < 0) { err(c, lineno, "continues nothing: ", w); return; }
		list_words(c, c->cont, line, lineno);
		return;
	}

	c->cont = -1;

	if (strcmp(w, "side") == 0) {
		static const char *const sides[] = { "bottom", "top", "left", "right" };
		if (!next_word(&s, w, 16, &too_long)) { err(c, lineno, "side: which?", ""); return; }
		for (int i = 0; i < 4; i++)
			if (strcmp(w, sides[i]) == 0) { c->side = (uint8_t)i; return; }
		err(c, lineno, "side: unknown: ", w);
		return;
	}

	if (strcmp(w, "dots") == 0) {
		if (!next_word(&s, w, 16, &too_long)) { err(c, lineno, "dots: on or off?", ""); return; }
		if (strcmp(w, "on") == 0) c->dots = true;
		else if (strcmp(w, "off") == 0) c->dots = false;
		else err(c, lineno, "dots: not on or off: ", w);
		return;
	}

	if (strcmp(w, "row") == 0) {
		int row;
		if (!next_word(&s, w, 16, &too_long)) { err(c, lineno, "row: wide or tall?", ""); return; }
		if (strcmp(w, "wide") == 0) row = DK_ROW_WIDE;
		else if (strcmp(w, "tall") == 0) row = DK_ROW_TALL;
		else { err(c, lineno, "row: not wide or tall: ", w); return; }
		// A list's names must be one run in the pool, so a second
		// `row wide` cannot add to the first.
		if (c->has[row]) { err(c, lineno, "given already: row ", w); return; }
		list_begin(c, row, s, lineno);
		return;
	}

	if (strcmp(w, "group") == 0) {
		char id[DK_GROUP_ID_MAX], name[DK_SPOKEN_MAX];
		if (!next_word(&s, id, sizeof(id), &too_long)) { err(c, lineno, "group: which?", ""); return; }
		if (too_long) { err(c, lineno, "group id too long: ", id); return; }
		if (id[0] == '@') { err(c, lineno, "group: drop the @: ", id); return; }
		if (dk_group_find(c, id) >= 0) { err(c, lineno, "given already: group ", id); return; }
		if (c->ngroups >= DK_GROUPS_MAX) { err(c, lineno, "too many groups: ", id); return; }
		if (!next_name(&s, name, sizeof(name), &too_long)) { err(c, lineno, "group: no name for ", id); return; }
		if (too_long) { err(c, lineno, "group name too long: ", name); return; }
		int g = c->ngroups++;
		strcpy(c->groups[g].id, id);
		strcpy(c->groups[g].name, name);
		list_begin(c, DK_ROWS + g, s, lineno);
		return;
	}

	err(c, lineno, "unknown setting: ", w);

}

void dk_cfg_text(dk_cfg_t *c, const char *text, uint32_t len) {

	char line[DK_LINE_MAX];
	uint32_t i = 0;
	int lineno = 0;

	while (i < len) {
		uint32_t n = 0;
		bool too_long = false;
		lineno++;
		while (i < len && text[i] != '\n') {
			if (n < sizeof(line) - 1) line[n++] = text[i];
			else too_long = true;
			i++;
		}
		i++;	// the newline
		line[n] = 0;
		if (too_long) err(c, lineno, "line too long, left out", "");
		else dk_cfg_line(c, line, lineno);
	}

}

int dk_side_row(int side) {
	return (side == DK_SIDE_LEFT || side == DK_SIDE_RIGHT) ? DK_ROW_TALL : DK_ROW_WIDE;
}

// -- the list --------------------------------------------------------

static const char *list_name(const dk_cfg_t *c, int list, int i) {
	const char *p = c->pool + c->start[list];
	while (i-- > 0) p += strlen(p) + 1;
	return p;
}

static bool list_names(const dk_cfg_t *c, int list, const char *name) {
	const char *p = c->pool + c->start[list];
	for (int i = 0; i < c->count[list]; i++) {
		if (strcmp(p, name) == 0) return true;
		p += strlen(p) + 1;
	}
	return false;
}

typedef struct {
	dk_item_t	*out;
	int		max;
	bool		full;
	dk_dock_t	*d;
} lister_t;

static void add(lister_t *L, const char *name, int zic, int view) {
	if (L->d->n >= L->max) { L->full = true; return; }
	dk_item_t *it = &L->out[L->d->n++];
	strncpy(it->name, name, DK_NAME_MAX - 1);
	it->name[DK_NAME_MAX - 1] = 0;
	it->zic = (int16_t)zic;
	it->view = (int8_t)view;
	it->icon_len = 0;
	it->icon_off = 0;
	it->spoken_off = DK_NONE;
}

// Placed by the file for this dock: named in the row or in one of the
// groups the row opens.
static bool placed(const dk_cfg_t *c, int row, const bool *opens, const char *name) {
	if (list_names(c, row, name)) return true;
	for (int g = 0; g < c->ngroups; g++)
		if (opens[g] && list_names(c, DK_ROWS + g, name)) return true;
	return false;
}

static void add_rest(lister_t *L, const dk_cfg_t *c, int row, const bool *opens,
	const dk_zic_t *zic) {
	if (!zic) return;
	for (int z = 0; z < zic->count; z++) {
		const char *name = dk_zic_name(zic, z);
		if ((zic->e[z].flags & DK_ZIC_HIDDEN) || name[0] == '@') continue;
		if (placed(c, row, opens, name)) continue;
		add(L, name, z, -1);
	}
}

// One list's names, @rest expanded where it stands (if it is the first
// @rest), a group's icon where a row names one.
static void add_list(lister_t *L, const dk_cfg_t *c, int row, int list, const bool *opens,
	const int *view_of, bool *rest_done, const dk_zic_t *zic) {

	for (int i = 0; i <= c->count[list]; i++) {

		if (i == c->rest[list] && !*rest_done) {
			*rest_done = true;
			add_rest(L, c, row, opens, zic);
		}

		if (i == c->count[list]) break;

		const char *p = list_name(c, list, i);
		if (p[0] == '@') {
			int g = dk_group_find(c, p + 1);
			if (g < 0 || view_of[g] < 0) continue;		// reported by dk_list()
			int z = dk_zic_find(zic, p);
			if (z < 0) z = dk_zic_find(zic, "@group");
			add(L, p + 1, z, view_of[g]);
		} else {
			add(L, p, dk_zic_find(zic, p), -1);
		}

	}

}

int dk_list(const dk_cfg_t *c, int row, const dk_zic_t *zic,
	dk_item_t *out, int max, dk_dock_t *d) {

	lister_t L = { out, max, false, d };
	bool opens[DK_GROUPS_MAX];
	int view_of[DK_GROUPS_MAX];
	bool rest_done = false;

	memset(d, 0, sizeof(*d));
	for (int g = 0; g < DK_GROUPS_MAX; g++) { opens[g] = false; view_of[g] = -1; }

	// Which groups the row opens, in the order it first names them:
	// that is the order of the views.
	for (int i = 0; i < c->count[row]; i++) {
		const char *p = list_name(c, row, i);
		if (p[0] != '@') continue;
		int g = dk_group_find(c, p + 1);
		if (g < 0) {
			if (c->report) c->report(c->ctx, 0, "no such group: ", p);
			continue;
		}
		if (opens[g]) continue;
		opens[g] = true;
		view_of[g] = d->nviews;
		d->views[d->nviews++].group = (uint8_t)g;
	}

	add_list(&L, c, row, row, opens, view_of, &rest_done, zic);
	d->nmain = d->n;

	for (int v = 0; v < d->nviews; v++) {
		d->views[v].start = (uint8_t)d->n;
		add_list(&L, c, row, DK_ROWS + d->views[v].group, opens, view_of,
			&rest_done, zic);
		d->views[v].count = (uint8_t)(d->n - d->views[v].start);
	}

	if (L.full && c->report)
		c->report(c->ctx, 0, "too many icons, the last left out", "");

	return d->n;

}

int dk_keep(dk_item_t *items, const uint8_t *keep, dk_dock_t *d) {

	int8_t newv[DK_GROUPS_MAX];
	int nv = 0, k = 0;

	// A view survives if any of its apps does.
	for (int v = 0; v < d->nviews; v++) {
		bool any = false;
		for (int i = d->views[v].start; i < d->views[v].start + d->views[v].count; i++)
			if (items[i].view < 0 && keep[i]) any = true;
		newv[v] = any ? (int8_t)nv++ : -1;
	}

	// Forward, in place: an item only ever moves down the list, and the
	// main row and the views keep their order.
	int nmain = 0;
	for (int i = 0; i < d->nmain; i++) {
		dk_item_t it = items[i];
		if (it.view >= 0) {
			if (newv[it.view] < 0) continue;
			it.view = newv[it.view];
		} else if (!keep[i]) continue;
		items[k++] = it;
		nmain++;
	}

	dk_view_t views[DK_GROUPS_MAX];
	for (int v = 0; v < d->nviews; v++) {
		if (newv[v] < 0) continue;
		dk_view_t nvw = d->views[v];
		int start = k;
		for (int i = d->views[v].start; i < d->views[v].start + d->views[v].count; i++)
			if (items[i].view < 0 && keep[i]) items[k++] = items[i];
		nvw.start = (uint8_t)start;
		nvw.count = (uint8_t)(k - start);
		views[newv[v]] = nvw;
	}

	memcpy(d->views, views, sizeof(dk_view_t) * (size_t)nv);
	d->nviews = nv;
	d->nmain = nmain;
	d->n = k;
	return k;

}

// -- running apps ----------------------------------------------------

bool dk_proc_is(const char *pname, const char *app) {
	if (!pname || !app || !app[0]) return false;
	while (*app && *pname == *app) { pname++; app++; }
	if (*app) return false;
	if (!*pname) return true;			// the program's own name
	while (*pname >= '0' && *pname <= '9') pname++;
	return !*pname;					// a registered one: base + digits
}
