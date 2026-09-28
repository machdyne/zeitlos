/*
 * Host test for sw/common/zjson.c. docs/zjson.md, "Testing".
 *
 *   make -C sw/common/tests -f Makefile.zjson
 *
 * zjson_cases.txt (gen_zjson_cases.py): 5,000 inputs -- hand-written
 * nasty ones, random documents, and random byte-level mutations of them
 * -- each with the verdict of Python's own json module plus the
 * subset's rules. Every verdict must agree; for every accepted input,
 * what the tokens say must re-serialize to exactly what json.dumps
 * wrote -- the same VALUES, not just the same yes or no.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../zjson.h"

static int checks, fails;
#define CK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char out[1 << 20];
static uint32_t on;

static void put(const char *s, uint32_t n) { memcpy(out + on, s, n); on += n; }

// as Python's json.dumps(ensure_ascii=False, separators=(",", ":"))
static void dump_str(const char *js, const zjson_tok_t *t) {
	static char buf[1 << 16];
	zjson_str(js, t, buf, sizeof(buf));
	put("\"", 1);
	for (uint32_t i = 0; i < t->end - t->start + 64 && buf[i]; i++) {
		unsigned char c = (unsigned char)buf[i];
		char e[8];
		switch (c) {
		case '"': put("\\\"", 2); break;
		case '\\': put("\\\\", 2); break;
		case '\n': put("\\n", 2); break;
		case '\r': put("\\r", 2); break;
		case '\t': put("\\t", 2); break;
		case '\b': put("\\b", 2); break;
		case '\f': put("\\f", 2); break;
		default:
			if (c < 0x20) { snprintf(e, sizeof(e), "\\u%04x", c); put(e, 6); }
			else put((char *)&c, 1);
		}
	}
	put("\"", 1);
}

static int dump(const char *js, const zjson_tok_t *tok, int i) {
	const zjson_tok_t *t = &tok[i];
	char num[32];
	switch (t->type) {
	case ZJ_OBJECT: case ZJ_ARRAY: {
		int obj = t->type == ZJ_OBJECT, j = i + 1;
		put(obj ? "{" : "[", 1);
		for (uint32_t k = 0; k < t->size; k++) {
			if (k) put(",", 1);
			if (obj) { dump_str(js, &tok[j]); put(":", 1); j++; }
			j = dump(js, tok, j);
		}
		put(obj ? "}" : "]", 1);
		return j;
	}
	case ZJ_STRING: dump_str(js, t); return i + 1;
	case ZJ_NUMBER: snprintf(num, sizeof(num), "%lld", (long long)t->num); put(num, (uint32_t)strlen(num)); return i + 1;
	case ZJ_TRUE: put("true", 4); return i + 1;
	case ZJ_FALSE: put("false", 5); return i + 1;
	default: put("null", 4); return i + 1;
	}
}

static int unhex(const char *h, char *b) {
	if (!strcmp(h, "-")) return 0;
	int n = 0;
	for (; h[0] && h[1]; h += 2) { unsigned v; sscanf(h, "%2x", &v); b[n++] = (char)v; }
	return n;
}

int main(void) {
	static zjson_tok_t tok[8192];
	static char line[1 << 17], in[1 << 16], want[1 << 16], h1[1 << 17], h2[1 << 17];
	FILE *f = fopen("zjson_cases.txt", "r");
	if (!f) { printf("no zjson_cases.txt\n"); return 1; }
	int ncase = 0, agree_bad = 0, value_bad = 0, accepted = 0;
	if (!fgets(line, sizeof(line), f)) return 1;
	ncase = atoi(line);
	for (int c = 0; c < ncase; c++) {
		int v;
		if (fscanf(f, "%d %s %s", &v, h1, h2) != 3) { printf("short file\n"); return 1; }
		int n = unhex(h1, in), wn = unhex(h2, want);
		uint32_t where = 0;
		int r = zjson_parse(in, (uint32_t)n, tok, 8192, &where);
		if ((r > 0) != (v == 1)) {
			agree_bad++;
			if (agree_bad <= 5) printf("  case %d: zjson %s (%s at %u), Python %s: %.*s\n", c, r > 0 ? "accepts" : "refuses",
				r > 0 ? "-" : zjson_strerror(r), where, v ? "accepts" : "refuses", n > 60 ? 60 : n, in);
			continue;
		}
		if (r > 0) {
			accepted++;
			on = 0;
			int end = dump(in, tok, 0);
			if (end != r || on != (uint32_t)wn || memcmp(out, want, on)) {
				value_bad++;
				if (value_bad <= 5) printf("  case %d: values differ: %.*s / %.*s\n", c, (int)on, out, wn, want);
			}
		}
	}
	CK(agree_bad == 0, "%d cases: %d verdicts differ from Python's", ncase, agree_bad);
	CK(value_bad == 0, "%d accepted: %d parsed to different values", accepted, value_bad);

	// the accessors
	{
		const char *js = "{\"name\":\"machdyne\",\"n\":-42,\"list\":[1,[2,3],{\"x\":null}],\"esc\":\"a\\u00e9\\ud83d\\ude00\"}";
		char s[64];
		int r = zjson_parse(js, (uint32_t)strlen(js), tok, 64, NULL);
		int name = zjson_get(js, tok, 0, "name"), n = zjson_get(js, tok, 0, "n"), l = zjson_get(js, tok, 0, "list");
		int e = zjson_get(js, tok, 0, "esc");
		CK(r > 0 && name > 0 && zjson_str(js, &tok[name], s, sizeof(s)) && !strcmp(s, "machdyne"), "get a string");
		CK(n > 0 && tok[n].type == ZJ_NUMBER && tok[n].num == -42, "get a number");
		CK(l > 0 && tok[l].type == ZJ_ARRAY && tok[l].size == 3 && zjson_next(tok, l) == e - 1, "zjson_next skips a nested array");
		CK(e > 0 && zjson_str(js, &tok[e], s, sizeof(s)) && !strcmp(s, "a\xc3\xa9\xf0\x9f\x98\x80"), "escapes decoded to UTF-8");
		CK(zjson_get(js, tok, 0, "absent") == -1 && !zjson_str(js, &tok[name], s, 5), "absent key; too small a buffer");
		CK(zjson_parse(js, (uint32_t)strlen(js), tok, 5, NULL) == -ZJ_E_TOKENS, "too few tokens is an error, not a crash");
	}

	printf("zjson: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
