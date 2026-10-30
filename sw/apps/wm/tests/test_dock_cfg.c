/*
 * Host test for the dock's two files (docs/dock.md, dock_cfg.h):
 * wm.cfg's parser, dock.zic's reader, and the list they make together.
 *
 *   cc -std=gnu99 -Wall -Wextra -o /tmp/t sw/apps/wm/tests/test_dock_cfg.c \
 *      sw/apps/wm/dock_cfg.c
 *   /tmp/t sw/data/wm.cfg sw/data/icons/dock.zic
 *
 * With the two real files as arguments it also holds the shipped ones
 * to what they promise: the built-in wm.cfg parses without a problem
 * and gives today's dock, and every app it names has an icon.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../dock_cfg.h"

static int checks, failures;
static dk_dock_t dk;

#define CHECK(cond) do { \
	checks++; \
	if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

#define CHECK_EQ(got, want) do { \
	long g_ = (long)(got), w_ = (long)(want); \
	checks++; \
	if (g_ != w_) { printf("FAIL %s:%d: %s == %ld, wanted %ld\n", \
		__FILE__, __LINE__, #got, g_, w_); failures++; } \
} while (0)

// -- reports --

static int nreports;
static int last_line;
static char last_msg[128];

static void report(void *ctx, int line, const char *msg, const char *word) {
	(void)ctx;
	nreports++;
	last_line = line;
	snprintf(last_msg, sizeof(last_msg), "%s%s", msg, word);
}

static void parse(dk_cfg_t *c, const char *text) {
	nreports = 0;
	last_line = 0;
	last_msg[0] = 0;
	dk_cfg_init(c, report, NULL);
	dk_cfg_text(c, text, (uint32_t)strlen(text));
}

static const char *row_name(const dk_cfg_t *c, int row, int i) {
	const char *p = c->pool + c->start[row];
	while (i-- > 0) p += strlen(p) + 1;
	return p;
}

// -- a small dock.zic, made here --

static uint8_t zbuf[16384];
static uint32_t zsize;
static dk_zic_t Z;			// its table, as wm reads it

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

// The generator's coding (gen_dock_icon_data.py), for the tests.
static int encode(const uint8_t *raw, uint8_t *out) {
	uint8_t x[DK_ICON_BYTES];
	int n = 0;
	for (int i = 0; i < DK_ICON_BYTES; i++) x[i] = raw[i] ^ (i >= 4 ? raw[i - 4] : 0);
	for (int i = 0; i < DK_ICON_BYTES; ) {
		int j = i;
		if (x[i] == 0) {
			while (j < DK_ICON_BYTES && x[j] == 0 && j - i < 128) j++;
			out[n++] = (uint8_t)(0x80 | (j - i - 1));
		} else {
			while (j < DK_ICON_BYTES && x[j] != 0 && j - i < 128) j++;
			out[n++] = (uint8_t)(j - i - 1);
			memcpy(out + n, x + i, (size_t)(j - i));
			n += j - i;
		}
		i = j;
	}
	return n;
}

// The icon entry `i` of make_zic() gets: its own pattern.
static void icon_of(int i, uint8_t *raw) {
	for (int k = 0; k < DK_ICON_BYTES; k++) raw[k] = (uint8_t)(k % 7 == 0 ? i + 1 : (k & 8 ? 0 : 0x81));
}

// names in order; a name ending in '!' is hidden. Spoken names "say N"
// (none for the hidden ones). Returns the size.
static uint32_t make_zic(const char *const *names, int n) {
	uint8_t spoken[2048], icons[8192];
	uint32_t ns = 0, ni = 0;
	memset(zbuf, 0, sizeof(zbuf));
	memcpy(zbuf, DK_ZIC_MAGIC, 4);
	put16(zbuf + 4, (uint32_t)n);
	put16(zbuf + 6, DK_ZIC_ENTRY);
	for (int i = 0; i < n; i++) {
		uint8_t *e = zbuf + DK_ZIC_HEADER + i * DK_ZIC_ENTRY;
		size_t l = strlen(names[i]);
		bool hidden = names[i][l - 1] == '!';
		memcpy(e, names[i], hidden ? l - 1 : l);
		put16(e + 24, hidden ? DK_ZIC_HIDDEN : 0);
		put16(e + 26, ns);
		if (hidden) spoken[ns++] = 0;
		else ns += (uint32_t)sprintf((char *)spoken + ns, "say %d", i) + 1;
		uint8_t raw[DK_ICON_BYTES];
		icon_of(i, raw);
		int len = encode(raw, icons + ni);
		put16(e + 28, ni);
		put16(e + 30, (uint32_t)len);
		ni += (uint32_t)len;
	}
	put32(zbuf + 8, ns);
	put32(zbuf + 12, ni);
	uint32_t at = DK_ZIC_HEADER + (uint32_t)n * DK_ZIC_ENTRY;
	memcpy(zbuf + at, spoken, ns);
	memcpy(zbuf + at + ns, icons, ni);
	zsize = at + ns + ni;
	return zsize;
}

// A file in memory, read `chunk` bytes at a time at most.
typedef struct { const uint8_t *data; uint32_t len, pos, chunk; int reads; } mem_t;

static int mem_read(void *ctx, void *buf, uint32_t n) {
	mem_t *m = (mem_t *)ctx;
	m->reads++;
	if (n > m->chunk) n = m->chunk;
	if (n > m->len - m->pos) n = m->len - m->pos;
	memcpy(buf, m->data + m->pos, n);
	m->pos += n;
	return (int)n;
}

static mem_t zm;
static uint8_t rbuf[512];

// make_zic() and read its table into Z, as wm would. Its count.
static int zic_of(const char *const *names, int n) {
	make_zic(names, n);
	zm = (mem_t){ zbuf, zsize, 0, 4096, 0 };
	return dk_zic_read_table(mem_read, &zm, &Z, rbuf, sizeof(rbuf), NULL);
}

static void *slurp(const char *path, long *len) {
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	*len = ftell(f);
	fseek(f, 0, SEEK_SET);
	void *buf = malloc((size_t)*len + 1);
	if (fread(buf, 1, (size_t)*len, f) != (size_t)*len) { fclose(f); free(buf); return NULL; }
	fclose(f);
	((char *)buf)[*len] = 0;
	return buf;
}

// -- the tests --

static bool no_gpio(uint32_t f1, uint32_t f2) { (void)f1; return !(f2 & 1); }

static void test_zic(void) {

	static const char *const names[] = { "@next!", "ask", "mmod", "poker!", "term" };
	uint8_t store[2048], out[DK_ICON_BYTES], raw[DK_ICON_BYTES];
	uint32_t used;

	printf("dock.zic: the icon coding\n");
	{
		uint8_t blob[256];
		int bad = 0;
		for (int t = 0; t < 400; t++) {
			srand((unsigned)t);
			for (int k = 0; k < DK_ICON_BYTES; k++)
				raw[k] = t < 3 ? (uint8_t)(t == 1 ? 0xff : t == 2 ? k : 0)
				       : (rand() % 3 ? 0 : (uint8_t)rand());
			int n = encode(raw, blob);
			if (n > 255 || !dk_icon_expand(blob, n, out) || memcmp(out, raw, DK_ICON_BYTES)) bad++;
			if (n > 1 && dk_icon_expand(blob, n - 1, out)) bad++;	// cut short
		}
		CHECK_EQ(bad, 0);
		uint8_t over[] = { 0xff, 0x80 };			// 129 zeros
		CHECK(!dk_icon_expand(over, 2, out));
		uint8_t lit[] = { 0x05, 1, 2 };				// says 6, has 2
		CHECK(!dk_icon_expand(lit, 3, out));
	}

	printf("dock.zic: the table\n");
	CHECK_EQ(zic_of(names, 5), 5);
	CHECK(strcmp(dk_zic_name(&Z, 1), "ask") == 0);
	CHECK_EQ(Z.e[3].flags, DK_ZIC_HIDDEN);
	CHECK_EQ(dk_zic_find(&Z, "term"), 4);
	CHECK_EQ(dk_zic_find(&Z, "@next"), 0);
	CHECK_EQ(dk_zic_find(&Z, "nope"), -1);
	CHECK_EQ(dk_zic_find(NULL, "term"), -1);
	CHECK_EQ(zm.pos, DK_ZIC_HEADER + 5 * DK_ZIC_ENTRY);	// no further than the table

	printf("dock.zic: the parts wanted, and only those\n");
	Z.e[1].flags |= DK_ZIC_WANT;
	Z.e[4].flags |= DK_ZIC_WANT;
	used = 3;					// what the store holds already stays
	memset(store, 0xee, sizeof(store));
	CHECK(dk_zic_read_parts(mem_read, &zm, &Z, rbuf, sizeof(rbuf), store, sizeof(store), &used));
	CHECK(strcmp((char *)store + Z.e[1].spoken, "say 1") == 0);
	CHECK(strcmp((char *)store + Z.e[4].spoken, "say 4") == 0);
	CHECK_EQ(Z.e[1].spoken, 3);
	CHECK(dk_icon_expand(store + Z.e[4].icon, Z.e[4].icon_len, out));
	icon_of(4, raw);
	CHECK(memcmp(out, raw, DK_ICON_BYTES) == 0);
	CHECK(dk_icon_expand(store + Z.e[1].icon, Z.e[1].icon_len, out));
	icon_of(1, raw);
	CHECK(memcmp(out, raw, DK_ICON_BYTES) == 0);
	CHECK(Z.e[0].icon_len == 0 && Z.e[0].spoken == DK_NONE);	// not wanted: none
	CHECK(Z.e[2].icon_len == 0 && Z.e[3].icon_len == 0);
	CHECK_EQ(used, 3 + 6 + 6 + Z.e[1].icon_len + Z.e[4].icon_len);
	CHECK_EQ(zm.pos, zsize);

	printf("dock.zic: any chunk size gives the same\n");
	for (uint32_t ch = 1; ch <= 64; ch = ch * 2 + 1) {
		make_zic(names, 5);
		mem_t m = { zbuf, zsize, 0, ch, 0 };
		uint8_t small[DK_ZIC_ENTRY + 3];
		dk_zic_t y;
		CHECK_EQ(dk_zic_read_table(mem_read, &m, &y, small, sizeof(small), NULL), 5);
		for (int i = 0; i < 5; i++) y.e[i].flags |= DK_ZIC_WANT;
		uint8_t st[2048];
		uint32_t u = 0;
		CHECK(dk_zic_read_parts(mem_read, &m, &y, small, sizeof(small), st, sizeof(st), &u));
		for (int i = 0; i < 5; i++) {
			icon_of(i, raw);
			CHECK(dk_icon_expand(st + y.e[i].icon, y.e[i].icon_len, out) &&
				memcmp(out, raw, DK_ICON_BYTES) == 0);
		}
		CHECK(y.e[0].spoken == DK_NONE && strcmp((char *)st + y.e[2].spoken, "say 2") == 0);
	}

	printf("dock.zic: hardware a board lacks\n");
	make_zic(names, 5);
	put32(zbuf + DK_ZIC_HEADER + 2 * DK_ZIC_ENTRY + 20, 1);	// mmod needs gpio
	zm = (mem_t){ zbuf, zsize, 0, 4096, 0 };
	CHECK_EQ(dk_zic_read_table(mem_read, &zm, &Z, rbuf, sizeof(rbuf), no_gpio), 5);
	CHECK(Z.e[2].flags & DK_ZIC_NOHW);
	CHECK(!(Z.e[1].flags & DK_ZIC_NOHW));

	printf("dock.zic: a full store keeps what fit\n");
	CHECK_EQ(zic_of(names, 5), 5);
	for (int i = 0; i < 5; i++) Z.e[i].flags |= DK_ZIC_WANT;
	used = 0;
	CHECK(!dk_zic_read_parts(mem_read, &zm, &Z, rbuf, sizeof(rbuf), store, 40, &used));
	CHECK(used <= 40);
	for (int i = 0; i < 5; i++)
		if (Z.e[i].icon_len) CHECK(dk_icon_expand(store + Z.e[i].icon, Z.e[i].icon_len, out));

	printf("dock.zic: damaged files are refused\n");
	{
		dk_zic_t y;
		struct { int at; uint32_t v; int bits; } bad[] = {
			{ 0, 'X', 8 },			// not a zic
			{ 6, 48, 16 },			// another entry layout
			{ 4, 0, 16 },			// no entries
			{ 4, DK_ZIC_ENTRIES + 1, 16 },	// too many
			{ DK_ZIC_HEADER + 26, 0x7000, 16 },	// a spoken name outside its section
			{ DK_ZIC_HEADER + 28, 0x7000, 16 },	// an icon outside its section
			{ DK_ZIC_HEADER + 30, 0, 16 },	// an icon of no length
			{ DK_ZIC_HEADER, 0, 8 },	// an empty name
		};
		for (unsigned b = 0; b < sizeof(bad) / sizeof(bad[0]); b++) {
			make_zic(names, 5);
			if (bad[b].bits == 8) zbuf[bad[b].at] = (uint8_t)bad[b].v;
			else put16(zbuf + bad[b].at, bad[b].v);
			mem_t m = { zbuf, zsize, 0, 4096, 0 };
			CHECK_EQ(dk_zic_read_table(mem_read, &m, &y, rbuf, sizeof(rbuf), NULL), -1);
		}
		make_zic(names, 5);
		memset(zbuf + DK_ZIC_HEADER, 'a', DK_NAME_MAX);		// a name with no end
		mem_t m = { zbuf, zsize, 0, 4096, 0 };
		CHECK_EQ(dk_zic_read_table(mem_read, &m, &y, rbuf, sizeof(rbuf), NULL), -1);
		make_zic(names, 5);
		m = (mem_t){ zbuf, 40, 0, 4096, 0 };			// cut short in the table
		CHECK_EQ(dk_zic_read_table(mem_read, &m, &y, rbuf, sizeof(rbuf), NULL), -1);
		make_zic(names, 5);
		m = (mem_t){ zbuf, zsize - 5, 0, 4096, 0 };		// cut short in the icons
		CHECK_EQ(dk_zic_read_table(mem_read, &m, &y, rbuf, sizeof(rbuf), NULL), 5);
		for (int i = 0; i < 5; i++) y.e[i].flags |= DK_ZIC_WANT;
		used = 0;
		CHECK(!dk_zic_read_parts(mem_read, &m, &y, rbuf, sizeof(rbuf), store, sizeof(store), &used));
		CHECK_EQ(y.e[4].icon_len, 0);				// the last one did not come
		CHECK(y.e[1].icon_len > 0);
		// out of order: a spoken name before the one before it
		make_zic(names, 5);
		put16(zbuf + DK_ZIC_HEADER + 4 * DK_ZIC_ENTRY + 26, 1);
		m = (mem_t){ zbuf, zsize, 0, 4096, 0 };
		CHECK_EQ(dk_zic_read_table(mem_read, &m, &y, rbuf, sizeof(rbuf), NULL), 5);
		for (int i = 0; i < 5; i++) y.e[i].flags |= DK_ZIC_WANT;
		used = 0;
		CHECK(!dk_zic_read_parts(mem_read, &m, &y, rbuf, sizeof(rbuf), store, sizeof(store), &used));
		CHECK_EQ(y.e[4].spoken, DK_NONE);
	}

}

static void test_parse(void) {

	dk_cfg_t c;

	printf("wm.cfg: the settings\n");
	parse(&c, "# a comment\n\nside bottom\nrow wide term files text\nrow tall term\n");
	CHECK_EQ(c.errors, 0);
	CHECK_EQ(c.side, DK_SIDE_BOTTOM);
	CHECK(c.has[DK_ROW_WIDE] && c.has[DK_ROW_TALL]);
	CHECK_EQ(c.count[DK_ROW_WIDE], 3);
	CHECK_EQ(c.count[DK_ROW_TALL], 1);
	CHECK(strcmp(row_name(&c, DK_ROW_WIDE, 2), "text") == 0);
	CHECK(strcmp(row_name(&c, DK_ROW_TALL, 0), "term") == 0);
	CHECK_EQ(c.rest[DK_ROW_WIDE], -1);

	parse(&c, "side left\n");
	CHECK_EQ(c.side, DK_SIDE_LEFT);
	parse(&c, "side top\n");
	CHECK_EQ(c.side, DK_SIDE_TOP);
	parse(&c, "side right   # comment\n");
	CHECK_EQ(c.side, DK_SIDE_RIGHT);
	CHECK_EQ(c.errors, 0);
	CHECK_EQ(dk_side_row(DK_SIDE_BOTTOM), DK_ROW_WIDE);
	CHECK_EQ(dk_side_row(DK_SIDE_TOP), DK_ROW_WIDE);
	CHECK_EQ(dk_side_row(DK_SIDE_LEFT), DK_ROW_TALL);
	CHECK_EQ(dk_side_row(DK_SIDE_RIGHT), DK_ROW_TALL);

	printf("wm.cfg: continuation lines, comments, CRLF\n");
	parse(&c, "row wide\n\tterm files  # the first two\n  text\r\n\n# between\n\tread\nside top\n\tweb\n");
	CHECK_EQ(c.count[DK_ROW_WIDE], 4);			// read: a blank line or a comment does not end the row
	CHECK(strcmp(row_name(&c, DK_ROW_WIDE, 2), "text") == 0);	// no \r left on it
	CHECK(strcmp(row_name(&c, DK_ROW_WIDE, 3), "read") == 0);
	CHECK_EQ(c.errors, 1);					// web continues `side`, not a row
	CHECK_EQ(last_line, 8);

	printf("wm.cfg: @rest\n");
	parse(&c, "row wide term @rest files\n");
	CHECK_EQ(c.count[DK_ROW_WIDE], 2);
	CHECK_EQ(c.rest[DK_ROW_WIDE], 1);
	parse(&c, "row wide @rest\n");
	CHECK_EQ(c.rest[DK_ROW_WIDE], 0);
	CHECK_EQ(c.count[DK_ROW_WIDE], 0);

	printf("wm.cfg: problems are reported with their line and the rest applies\n");
	parse(&c, "colour blue\nrow wide term\n");
	CHECK_EQ(c.errors, 1);
	CHECK_EQ(last_line, 1);
	CHECK(strstr(last_msg, "unknown setting: colour"));
	CHECK_EQ(c.count[DK_ROW_WIDE], 1);

	parse(&c, "dots off\n");
	CHECK_EQ(c.errors, 0);
	CHECK(!c.dots);
	parse(&c, "dots on\n");
	CHECK(c.dots);
	parse(&c, "row wide term\n");
	CHECK(c.dots);					// on unless said
	parse(&c, "dots maybe\n");
	CHECK_EQ(c.errors, 1);
	CHECK(c.dots);
	parse(&c, "dots\n");
	CHECK_EQ(c.errors, 1);

	parse(&c, "side middle\n");
	CHECK_EQ(c.errors, 1);
	CHECK_EQ(c.side, DK_SIDE_BOTTOM);
	parse(&c, "side\n");
	CHECK_EQ(c.errors, 1);
	parse(&c, "row\n");
	CHECK_EQ(c.errors, 1);
	parse(&c, "row long term\n");
	CHECK_EQ(c.errors, 1);
	CHECK(!c.has[DK_ROW_WIDE]);

	parse(&c, "row wide term @games files\n");	// a group not given: found out by dk_list()
	CHECK_EQ(c.errors, 0);
	CHECK_EQ(c.count[DK_ROW_WIDE], 3);
	{
		dk_item_t it[DK_ITEMS_MAX];
		nreports = 0;
		int n = dk_list(&c, DK_ROW_WIDE, NULL, it, DK_ITEMS_MAX, &dk);
		CHECK_EQ(n, 2);
		CHECK_EQ(nreports, 1);
		CHECK(strstr(last_msg, "no such group: @games"));
	}

	parse(&c, "row wide @rest term @rest\n");
	CHECK_EQ(c.errors, 1);
	CHECK_EQ(c.rest[DK_ROW_WIDE], 0);

	parse(&c, "row wide term averyveryverylongname files\n");
	CHECK_EQ(c.errors, 1);
	CHECK_EQ(c.count[DK_ROW_WIDE], 2);

	parse(&c, "\tterm\n");
	CHECK_EQ(c.errors, 1);
	CHECK(strstr(last_msg, "continues nothing"));

	parse(&c, "row wide term\nrow wide files\n\ttext\n");
	CHECK_EQ(c.errors, 2);			// the second row wide, and its continuation
	CHECK_EQ(c.count[DK_ROW_WIDE], 1);

	{
		char big[400];
		memset(big, 'x', sizeof(big));
		memcpy(big, "row wide ", 9);
		big[sizeof(big) - 1] = 0;
		char text[600];
		snprintf(text, sizeof(text), "%s\nrow tall term\n", big);
		parse(&c, text);
		CHECK_EQ(c.errors, 1);
		CHECK_EQ(last_line, 1);
		CHECK(!c.has[DK_ROW_WIDE]);		// the long line left out whole
		CHECK_EQ(c.count[DK_ROW_TALL], 1);
	}

	printf("wm.cfg: names beyond the pool are left out, not overrun\n");
	{
		char text[4000] = "row wide";
		for (int i = 0; i < 200; i++) {
			char w[24];
			snprintf(w, sizeof(w), "\n\tapp%03d", i);
			strcat(text, w);
		}
		parse(&c, text);
		CHECK(c.used <= DK_POOL);
		CHECK(c.errors > 0);
		CHECK(c.count[DK_ROW_WIDE] > 50);
	}

	printf("wm.cfg: a last line with no newline\n");
	parse(&c, "side top\nrow wide\n\tterm files\n\ttext @rest\nrow tall read");
	CHECK_EQ(c.errors, 0);
	CHECK_EQ(c.side, DK_SIDE_TOP);
	CHECK_EQ(c.count[DK_ROW_WIDE], 3);
	CHECK_EQ(c.count[DK_ROW_TALL], 1);
	CHECK_EQ(c.rest[DK_ROW_WIDE], 3);

}

static void test_list(void) {

	static const char *const names[] = {
		"@next!", "ask", "calc", "files", "poker!", "term", "text"
	};
	int zn = zic_of(names, 7);
	dk_cfg_t c;
	dk_item_t it[DK_ITEMS_MAX];
	int n;

	CHECK_EQ(zn, 7);

	printf("list: the row, in its order, with icons\n");
	parse(&c, "row wide text term nothere\n");
	n = dk_list(&c, DK_ROW_WIDE, &Z, it, DK_ITEMS_MAX, &dk);
	CHECK_EQ(n, 3);
	CHECK(strcmp(it[0].name, "text") == 0);
	CHECK_EQ(it[0].zic, 6);
	CHECK_EQ(it[1].zic, 5);
	CHECK_EQ(it[2].zic, -1);			// no icon: drawn as its name

	printf("list: @rest is what the row leaves out, in name order, hidden ones not\n");
	parse(&c, "row wide term @rest text\n");
	n = dk_list(&c, DK_ROW_WIDE, &Z, it, DK_ITEMS_MAX, &dk);
	CHECK_EQ(n, 5);
	CHECK(strcmp(it[0].name, "term") == 0);
	CHECK(strcmp(it[1].name, "ask") == 0);
	CHECK(strcmp(it[2].name, "calc") == 0);
	CHECK(strcmp(it[3].name, "files") == 0);
	CHECK(strcmp(it[4].name, "text") == 0);

	parse(&c, "row wide @rest\n");
	n = dk_list(&c, DK_ROW_WIDE, &Z, it, DK_ITEMS_MAX, &dk);
	CHECK_EQ(n, 5);					// 7 less @next and poker

	parse(&c, "row wide poker @rest\n");		// named, a hidden one shows
	n = dk_list(&c, DK_ROW_WIDE, &Z, it, DK_ITEMS_MAX, &dk);
	CHECK_EQ(n, 6);
	CHECK(strcmp(it[0].name, "poker") == 0);

	printf("list: the other row\n");
	parse(&c, "row wide term\nrow tall files calc\n");
	n = dk_list(&c, DK_ROW_TALL, &Z, it, DK_ITEMS_MAX, &dk);
	CHECK_EQ(n, 2);
	CHECK(strcmp(it[1].name, "calc") == 0);

	printf("list: no dock.zic\n");
	parse(&c, "row wide term files @rest\n");
	n = dk_list(&c, DK_ROW_WIDE, NULL, it, DK_ITEMS_MAX, &dk);
	CHECK_EQ(n, 2);
	CHECK_EQ(it[0].zic, -1);
	CHECK_EQ(it[1].zic, -1);

	printf("list: more than the dock holds\n");
	{
		char text[2000] = "row wide";
		for (int i = 0; i < 60; i++) {
			char w[16];
			snprintf(w, sizeof(w), "\n\ta%d", i);
			strcat(text, w);
		}
		parse(&c, text);
		nreports = 0;
		n = dk_list(&c, DK_ROW_WIDE, &Z, it, DK_ITEMS_MAX, &dk);
		CHECK_EQ(n, DK_ITEMS_MAX);
		CHECK_EQ(nreports, 1);			// said once
		CHECK(strcmp(it[DK_ITEMS_MAX - 1].name, "a47") == 0);
		n = dk_list(&c, DK_ROW_WIDE, &Z, it, 5, &dk);
		CHECK_EQ(n, 5);
	}

	printf("list: keeping what can run\n");
	{
		parse(&c, "row wide ask calc files term text\n");
		n = dk_list(&c, DK_ROW_WIDE, &Z, it, DK_ITEMS_MAX, &dk);
		uint8_t keep[] = { 1, 0, 1, 0, 1 };
		n = dk_keep(it, keep, &dk);
		CHECK_EQ(n, 3);
		CHECK(strcmp(it[0].name, "ask") == 0);
		CHECK(strcmp(it[1].name, "files") == 0);
		CHECK(strcmp(it[2].name, "text") == 0);
		CHECK_EQ(it[2].zic, 6);
	}

}

static void test_groups(void) {

	static const char *const names[] = {
		"@games", "@group!", "@next!", "ask", "calc", "chess", "files",
		"play!", "snake", "term", "text", "track"
	};
	int zn = zic_of(names, 12);
	CHECK_EQ(zn, 12);
	dk_cfg_t c;
	dk_item_t it[DK_ITEMS_MAX];
	int n;

	printf("groups: parsing\n");
	parse(&c, "group games Games chess snake\n"
		"group sound \"Sound and music\" track\n\tplay\n"
		"row wide term @games text @sound\n");
	CHECK_EQ(c.errors, 0);
	CHECK_EQ(c.ngroups, 2);
	CHECK(strcmp(c.groups[0].id, "games") == 0 && strcmp(c.groups[0].name, "Games") == 0);
	CHECK(strcmp(c.groups[1].name, "Sound and music") == 0);
	CHECK_EQ(c.count[DK_ROWS + 1], 2);			// continued on the next line
	CHECK_EQ(dk_group_find(&c, "sound"), 1);
	CHECK_EQ(dk_group_find(&c, "nope"), -1);

	printf("groups: the main row, then a view per group, in the row's order\n");
	n = dk_list(&c, DK_ROW_WIDE, &Z, it, DK_ITEMS_MAX, &dk);
	CHECK_EQ(dk.nmain, 4);
	CHECK_EQ(dk.nviews, 2);
	CHECK_EQ(n, 4 + 2 + 2);
	CHECK(strcmp(it[1].name, "games") == 0 && it[1].view == 0);
	CHECK_EQ(it[1].zic, 0);				// its own icon, @games
	CHECK(strcmp(it[3].name, "sound") == 0 && it[3].view == 1);
	CHECK_EQ(it[3].zic, 1);				// no @sound: the plain @group
	CHECK(it[0].view == -1 && it[2].view == -1);
	CHECK(dk.views[0].start == 4 && dk.views[0].count == 2 && dk.views[0].group == 0);
	CHECK(strcmp(it[4].name, "chess") == 0 && strcmp(it[5].name, "snake") == 0);
	CHECK(dk.views[1].start == 6 && dk.views[1].count == 2 && dk.views[1].group == 1);
	CHECK(strcmp(it[7].name, "play") == 0);

	printf("groups: a group named twice is one view; one never named is no view\n");
	parse(&c, "group games Games chess\ngroup unused Unused ask\nrow wide @games term @games\n");
	n = dk_list(&c, DK_ROW_WIDE, &Z, it, DK_ITEMS_MAX, &dk);
	CHECK_EQ(dk.nmain, 3);
	CHECK_EQ(dk.nviews, 1);
	CHECK(it[0].view == 0 && it[2].view == 0);
	CHECK_EQ(n, 4);

	printf("groups: @rest in a group: what the row and its groups leave out\n");
	parse(&c, "row wide term @games @misc\n"
		"group games Games chess snake\ngroup misc Misc @rest\n");
	n = dk_list(&c, DK_ROW_WIDE, &Z, it, DK_ITEMS_MAX, &dk);
	CHECK_EQ(dk.nviews, 2);
	CHECK_EQ(dk.views[1].count, 5);			// ask calc files text track: not chess, snake, term, hidden play
	CHECK(strcmp(it[dk.views[1].start].name, "ask") == 0);
	CHECK(strcmp(it[dk.views[1].start + 4].name, "track") == 0);

	printf("groups: @rest only where it first stands\n");
	parse(&c, "row wide term @rest @misc\ngroup misc Misc @rest\n");
	n = dk_list(&c, DK_ROW_WIDE, &Z, it, DK_ITEMS_MAX, &dk);
	CHECK_EQ(dk.nmain, 1 + 7 + 1);			// term, the seven others, misc
	CHECK_EQ(dk.views[0].count, 0);			// empty: dk_keep() drops it

	printf("groups: problems\n");
	parse(&c, "group games Games chess @misc\n");
	CHECK_EQ(c.errors, 1);
	CHECK(strstr(last_msg, "cannot hold a group"));
	CHECK_EQ(c.count[DK_ROWS], 1);
	parse(&c, "group games Games chess\ngroup games Again snake\n");
	CHECK_EQ(c.errors, 1);
	CHECK(strstr(last_msg, "given already"));
	parse(&c, "group\n");
	CHECK_EQ(c.errors, 1);
	parse(&c, "group games\n");
	CHECK_EQ(c.errors, 1);
	CHECK(strstr(last_msg, "no name"));
	parse(&c, "group @games Games\n");
	CHECK_EQ(c.errors, 1);
	parse(&c, "group averyverylongid Games\n");
	CHECK_EQ(c.errors, 1);
	parse(&c, "group g \"a name far too long to be said at all\" chess\n");
	CHECK_EQ(c.errors, 1);
	parse(&c, "row wide @\n");
	CHECK_EQ(c.errors, 1);
	{
		char text[2000] = "";
		for (int g = 0; g < DK_GROUPS_MAX + 2; g++) {
			char l[48];
			snprintf(l, sizeof(l), "group g%d G%d chess\n", g, g);
			strcat(text, l);
		}
		parse(&c, text);
		CHECK_EQ(c.ngroups, DK_GROUPS_MAX);
		CHECK_EQ(c.errors, 2);
	}

	printf("groups: keeping what can run, and dropping what is left empty\n");
	parse(&c, "row wide term @games text @sound calc\n"
		"group games Games chess snake\ngroup sound Sound track play\n");
	n = dk_list(&c, DK_ROW_WIDE, &Z, it, DK_ITEMS_MAX, &dk);
	CHECK_EQ(n, 5 + 2 + 2);
	{
		// term games text sound calc | chess snake | track play
		uint8_t keep[] = { 1, 1, 0, 1, 1,  0, 1,  0, 0 };
		n = dk_keep(it, keep, &dk);
		CHECK_EQ(dk.nmain, 3);			// term, games, calc: text not kept, sound empty
		CHECK(strcmp(it[0].name, "term") == 0);
		CHECK(strcmp(it[1].name, "games") == 0 && it[1].view == 0);
		CHECK(strcmp(it[2].name, "calc") == 0);
		CHECK_EQ(dk.nviews, 1);
		CHECK(dk.views[0].start == 3 && dk.views[0].count == 1);
		CHECK(strcmp(it[3].name, "snake") == 0);
		CHECK_EQ(n, 4);
	}
	n = dk_list(&c, DK_ROW_WIDE, &Z, it, DK_ITEMS_MAX, &dk);
	{
		// the first group empty, the second kept: the view renumbered
		uint8_t keep[] = { 1, 1, 1, 1, 1,  0, 0,  1, 0 };
		n = dk_keep(it, keep, &dk);
		CHECK_EQ(dk.nmain, 4);			// term text sound calc
		CHECK(strcmp(it[2].name, "sound") == 0 && it[2].view == 0);
		CHECK_EQ(dk.nviews, 1);
		CHECK_EQ(dk.views[0].group, 1);
		CHECK(strcmp(it[dk.views[0].start].name, "track") == 0);
	}
	n = dk_list(&c, DK_ROW_WIDE, &Z, it, DK_ITEMS_MAX, &dk);
	{
		uint8_t keep[16];
		memset(keep, 1, sizeof(keep));
		int before = n;
		n = dk_keep(it, keep, &dk);
		CHECK_EQ(n, before);			// all kept: unchanged
		CHECK_EQ(dk.nviews, 2);
	}

	printf("groups: the list cap counts the groups' members too\n");
	{
		char text[3000] = "row wide @big\ngroup big Big";
		for (int i = 0; i < 60; i++) {
			char w[16];
			snprintf(w, sizeof(w), "\n\tb%d", i);
			strcat(text, w);
		}
		parse(&c, text);
		nreports = 0;
		n = dk_list(&c, DK_ROW_WIDE, &Z, it, DK_ITEMS_MAX, &dk);
		CHECK_EQ(n, DK_ITEMS_MAX);
		CHECK_EQ(dk.views[0].count, DK_ITEMS_MAX - 1);
		CHECK_EQ(nreports, 1);
	}

}

static void test_running(void) {
	printf("running: which process is which app\n");
	CHECK(dk_proc_is("text", "text"));		// the program's name
	CHECK(dk_proc_is("web0", "web"));		// a registered name
	CHECK(dk_proc_is("ask12", "ask"));
	CHECK(!dk_proc_is("text", "term"));
	CHECK(!dk_proc_is("textx", "text"));
	CHECK(!dk_proc_is("tex", "text"));
	CHECK(!dk_proc_is("", "text"));			// the kernel's own
	CHECK(!dk_proc_is("text", ""));
	CHECK(!dk_proc_is(NULL, "text"));
	CHECK(dk_proc_is("chip8", "chip8"));
	CHECK(dk_proc_is("space3d", "space3d"));
	CHECK(!dk_proc_is("space3d", "space"));		// 3d is not a number
	CHECK(!dk_proc_is("repl0", "re"));
}

// The shipped files, when given.
static void test_shipped(const char *cfg_path, const char *zic_path) {

	long clen = 0, zlen = 0;
	char *text = slurp(cfg_path, &clen);
	uint8_t *zic = slurp(zic_path, &zlen);
	dk_cfg_t c;
	dk_item_t it[DK_ITEMS_MAX];
	static uint8_t store[8192];
	uint32_t used = 0;

	printf("shipped: %s and %s\n", cfg_path, zic_path);
	CHECK(text != NULL);
	CHECK(zic != NULL);
	if (!text || !zic) return;

	mem_t m = { zic, (uint32_t)zlen, 0, 1000, 0 };
	int zn = dk_zic_read_table(mem_read, &m, &Z, rbuf, sizeof(rbuf), NULL);
	CHECK(zn > 40);
	CHECK(dk_zic_header(zic, (uint32_t)zlen, &(dk_zic_t){0}));	// the size adds up
	CHECK(dk_zic_find(&Z, "@next") >= 0 && dk_zic_find(&Z, "@back") >= 0);
	CHECK(dk_zic_find(&Z, "@group") >= 0);
	for (int i = 1; i < zn; i++)			// name order: what @rest relies on
		CHECK(strcmp(dk_zic_name(&Z, i - 1), dk_zic_name(&Z, i)) < 0);
	for (int i = 0; i < zn; i++) Z.e[i].flags |= DK_ZIC_WANT;
	CHECK(dk_zic_read_parts(mem_read, &m, &Z, rbuf, sizeof(rbuf), store, sizeof(store), &used));
	int good = 0;
	uint32_t icon_bytes = 0;
	for (int i = 0; i < zn; i++) {
		uint8_t out[DK_ICON_BYTES];
		if (dk_icon_expand(store + Z.e[i].icon, Z.e[i].icon_len, out)) good++;
		icon_bytes += Z.e[i].icon_len;
	}
	CHECK_EQ(good, zn);				// every icon expands
	printf("  %d icons, %u bytes of them (%u%% of %d x 128)\n", zn, icon_bytes,
		(unsigned)(icon_bytes * 100 / (uint32_t)(zn * DK_ICON_BYTES)), zn);
	int t = dk_zic_find(&Z, "term");
	CHECK(strcmp((char *)store + Z.e[t].spoken, "Terminal") == 0);
	CHECK_EQ(Z.e[dk_zic_find(&Z, "casino")].spoken, DK_NONE);

	// Read again for the list (the parts changed the table's offsets).
	m = (mem_t){ zic, (uint32_t)zlen, 0, 1000, 0 };
	dk_zic_read_table(mem_read, &m, &Z, rbuf, sizeof(rbuf), NULL);

	parse(&c, text);
	CHECK_EQ(c.errors, 0);
	CHECK_EQ(c.side, DK_SIDE_BOTTOM);
	CHECK(c.dots);
	CHECK(c.has[DK_ROW_WIDE]);
	CHECK(c.has[DK_ROW_TALL]);

	// The wide row: 12 apps and the Network, Sound, Tools, Games and
	// More apps groups -- 17, the dock's width. More apps holds colwin
	// and @rest: here settings, which no group names, and view, whose
	// group (Work) the wide row does not show.
	static const char *const row[] = {
		"term", "files", "text", "read", "sheet", "slide", "draw", "learn",
		"ask", "calc", "cal", "clock", "net", "sound", "tools", "games", "misc",
	};
	int n = dk_list(&c, DK_ROW_WIDE, &Z, it, DK_ITEMS_MAX, &dk);
	CHECK_EQ(dk.nmain, 17);
	for (int i = 0; i < dk.nmain && i < 17; i++) {
		CHECK(strcmp(it[i].name, row[i]) == 0);
		CHECK(it[i].zic >= 0);			// every one with an icon
	}
	CHECK_EQ(dk.nviews, 5);
	CHECK_EQ(dk.views[0].count, 2);			// web irc
	CHECK_EQ(dk.views[1].count, 3);			// play track midi
	CHECK_EQ(dk.views[2].count, 7);			// backup info keyboard hex basic mmod pix
	CHECK_EQ(dk.views[3].count, 11);			// the games, and planedemo
	CHECK_EQ(dk.views[4].count, 3);			// colwin, and @rest: settings view
	{
		uint8_t keep[DK_ITEMS_MAX];
		memset(keep, 1, sizeof(keep));
		n = dk_keep(it, keep, &dk);
		CHECK_EQ(dk.nmain, 17);			// More apps has something: kept
		CHECK_EQ(dk.nviews, 5);
		CHECK_EQ(n, 17 + 2 + 3 + 7 + 11 + 3);
		int apps = 0;
		for (int i = 0; i < n; i++) if (it[i].view < 0) apps++;
		CHECK_EQ(apps, 38);
	}
	CHECK(strcmp(c.groups[dk.views[0].group].name, "Network") == 0);
	CHECK(strcmp(c.groups[dk.views[1].group].name, "Sound") == 0);
	CHECK(strcmp(c.groups[dk.views[2].group].name, "Tools") == 0);
	CHECK(strcmp(c.groups[dk.views[4].group].name, "More apps") == 0);

	// The tall row: 5 apps and 7 groups, 12, the height of a dock on a
	// side.
	n = dk_list(&c, DK_ROW_TALL, &Z, it, DK_ITEMS_MAX, &dk);
	{
		uint8_t keep[DK_ITEMS_MAX];
		memset(keep, 1, sizeof(keep));
		n = dk_keep(it, keep, &dk);
		CHECK_EQ(dk.nmain, 12);
		int apps = 0;
		for (int i = 0; i < n; i++) if (it[i].view < 0) apps++;
		CHECK_EQ(apps, 38);
	}

	// Every app the file names has an icon, and every group its own.
	for (int g = 0; g < c.ngroups; g++) {
		char at[DK_GROUP_ID_MAX + 1];
		snprintf(at, sizeof(at), "@%s", c.groups[g].id);
		CHECK(dk_zic_find(&Z, at) >= 0);
	}

	free(text);
	free(zic);

}

// A cursor file (docs/cursor.md): two 16x16 sprites in a .zsp sheet.
static void test_cursor(void) {

	uint16_t n[16], b[16];
	int line;
	char f[2048];
	int k = 0;

	printf("cursor files\n");
	k += sprintf(f + k, "zsp 1\r\ncolours 2\n# a comment\nsprite other 4 4\n....\n");
	k += sprintf(f + k, "sprite normal 16 16\n");
	for (int r = 0; r < 16; r++) {
		char row[17];
		for (int c = 0; c < 16; c++) row[c] = (c == r) ? '1' : (c == 15 - r ? '#' : '.');
		row[16] = 0;
		k += sprintf(f + k, "\t%s\r\n", row);		// indented, CRLF
	}
	k += sprintf(f + k, "anim spin 80 normal busy\nsprite busy 16 16 hot 8 8\n");
	for (int r = 0; r < 16; r++)
		k += sprintf(f + k, "%s\n", r == 8 ? "0000000011111111" : "................");
	CHECK(dk_cursor_parse(f, (uint32_t)k, n, b, &line));
	bool diag = true;
	for (int r = 0; r < 16; r++)
		if (n[r] != (uint16_t)((1u << r) | (1u << (15 - r)))) diag = false;
	CHECK(diag);					// any mark but '.' and '0' inverts
	CHECK(b[8] == 0xff00 && b[0] == 0 && b[15] == 0);	// '0' leaves it alone
	CHECK(line == 0);

	// a row of 15
	char g[2048];
	memcpy(g, f, (size_t)k);
	char *row7 = strstr(g, "sprite busy");
	for (int i = 0; i < 8; i++) row7 = strchr(row7, '\n') + 1;	// busy's row 7
	row7[15] = '\n';
	row7[16] = '.';
	CHECK(!dk_cursor_parse(g, (uint32_t)k, n, b, &line) && line > 0);

	// no busy at all
	char *cut = strstr(f, "anim spin");
	CHECK(!dk_cursor_parse(f, (uint32_t)(cut - f), n, b, &line) && line == 0);

	// cut short in the middle of busy's rows
	CHECK(!dk_cursor_parse(f, (uint32_t)k - 40, n, b, &line) && line > 0);

	// nothing
	CHECK(!dk_cursor_parse("", 0, n, b, &line) && line == 0);

}

int main(int argc, char **argv) {

	test_zic();
	test_parse();
	test_list();
	test_groups();
	test_running();
	test_cursor();
	if (argc >= 3) test_shipped(argv[1], argv[2]);

	printf("test_dock_cfg: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;

}
