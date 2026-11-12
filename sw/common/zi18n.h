#ifndef ZI18N_H
#define ZI18N_H
/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Languages: a program's text, translated. See docs/i18n.md.
 *
 * The English stays in the source, as the key and as the fallback:
 *
 *     z_tr_init(Z_APP_NAME);                   // first thing in main()
 *     ...
 *     z_fb_draw_utf8(x, y, Z_("Save as..."), 1, font, &clip);
 *     z_win_create(ZS_("Files"), ...);         // drawn in the small font
 *     snprintf(b, sizeof b, Z_N("%d file", "%d files", n), n);
 *     button(Z_C("verb", "Open"));
 *     static const char *const items[] = { N_("Open"), N_("Save") };
 *     static const char *const months[] = { NC_("month", "May"), ... };
 *     ... Z_D(items[i]) ...                    // a string chosen at run time
 *
 *   Z_(s)       the translation of the literal s, or s
 *   ZS_(s)      the same, but only if every character of the translation
 *               can be drawn in z_font_5x8 (window titles, the dock,
 *               anything small): Japanese has no glyphs that small, so
 *               there it stays English
 *   ZL_(s)      the same, but only if the translation is all Latin-9:
 *               for text drawn through a path that has only the hardware
 *               glyphs (wm's captions)
 *   ZS_D(s), ZL_D(s), Z_DC(c, s)  the same for a string from a table
 *   Z_C(c, s)   s in context c, for one English word that is two in
 *               another language ("Open" the verb, "Open" the state)
 *   Z_N(a, b, n) the singular a or plural b, by the language's rule for n
 *               (ZS_N: and only where it fits the small font)
 *   Z_D(s)      a string that is not a literal here (from a table marked
 *               with N_()); looked up every time, never cached
 *   N_(s)       marks a literal for translation without translating it
 *   NC_(c, s)   the same, in context c
 *
 * The cost:
 *   - In English, or in a program that never calls z_tr_init(), each
 *     Z_() is one load and one branch after its first use -- the literal
 *     itself, cached at the call site. Nothing is read, searched or
 *     allocated.
 *   - In another language, each call site looks its string up once (a
 *     hash and a binary search in the catalog jfont holds, zlang.h) and
 *     keeps the answer: a window redrawn a thousand times searches once.
 *   - A program that cannot afford even that -- a game's inner loop --
 *     simply does not call z_tr_init(), and is English.
 *   - In the small build (Z_SMALL, Obst) all of it is the plain string:
 *     Obst speaks English, at no cost.
 *
 * The language is read once, by z_tr_init(), from zeitlos.cfg's
 * system.language. A string asked for before z_tr_init() is English for
 * the life of the program, so call it first.
 */

#include <stdint.h>
#include <stdbool.h>

#define N_(s)          (s)

// A program built outside app.mk -- a host test -- has no Z_APP_NAME.
#ifndef Z_APP_NAME
#define Z_APP_NAME "common"
#endif

// English, at no cost: the small build (Obst), and a host test of an
// app, which links no lookup -- only zi18n.c's own test asks for one
// (Z_I18N_TEST).
#if defined(Z_SMALL) || (!defined(__riscv) && !defined(Z_I18N_TEST))
#define Z_TR_ENGLISH_ONLY 1
#endif

#ifdef Z_TR_ENGLISH_ONLY

#define Z_(s)          (s)
#define ZS_(s)         (s)
#define ZL_(s)         (s)
#define Z_C(c, s)      (s)
#define Z_N(a, b, n)   ((n) == 1 ? (a) : (b))
#define ZS_N(a, b, n)  ((n) == 1 ? (a) : (b))
#define Z_D(s)         (s)
#define ZS_D(s)        (s)
#define ZL_D(s)        (s)
#define Z_DC(c, s)     (s)
#define ZS_DC(c, s)    (s)
#define NC_(c, s)      (s)
static inline void z_tr_init(const char *app) { (void)app; }
static inline const char *z_tr_lang(void) { return "en"; }
static inline bool z_tr_fits_small(const char *s) { (void)s; return true; }

#else

// The call-site cache: the answer, once there is a final one. One call
// per use -- the cache is checked first thing inside it -- because that
// is half the code an inline check takes at each of a program's
// hundreds of sites, and flash is what the core apps are short of.
#define Z_TR_SITE(f, s) ({ static const char *z__tr_c; f(&z__tr_c, (s)); })

#define Z_(s)          Z_TR_SITE(z_tr_at, (s))
#define ZS_(s)         Z_TR_SITE(z_tr_at_small, (s))
#define ZL_(s)         Z_TR_SITE(z_tr_at_l9, (s))
#define Z_C(c, s)      ({ static const char *z__tr_c; z_tr_at_c(&z__tr_c, (c), (s)); })
#define Z_N(a, b, n)   z_tr_n((a), (b), (long)(n))
#define ZS_N(a, b, n)  z_tr_nfit((a), (b), (long)(n), Z_TR_SMALL)
#define Z_D(s)         z_tr((s))
#define ZS_D(s)        z_tr_fit((s), Z_TR_SMALL)
#define ZL_D(s)        z_tr_fit((s), Z_TR_L9)
#define Z_DC(c, s)     z_tr_c((c), (s))
#define ZS_DC(c, s)    z_tr_cfit((c), (s), Z_TR_SMALL)
#define NC_(c, s)      (s)

// What a translation must be drawable in to be used (Z_TR_SITE).
#define Z_TR_ANY       0
#define Z_TR_SMALL     1	// z_font_5x8: Latin-9, or jfont's 5x8 glyphs
#define Z_TR_L9        2	// Latin-9 only

// Reads system.language and remembers `app` (Z_APP_NAME) as the
// catalog's first domain; "common" (the shared library's strings) is
// searched after it.
void z_tr_init(const char *app);

// The language: "en", or the code in system.language ("de", "ja").
const char *z_tr_lang(void);

const char *z_tr(const char *s);
const char *z_tr_c(const char *ctx, const char *s);
const char *z_tr_n(const char *one, const char *many, long n);
const char *z_tr_small(const char *s);
const char *z_tr_fit(const char *s, int fit);
const char *z_tr_cfit(const char *ctx, const char *s, int fit);
const char *z_tr_nfit(const char *one, const char *many, long n, int fit);

// Z_TR_SITE's: the answer for s, from *cache if it has one, else looked
// up and stored there when it is final (English, or found in a catalog
// that is loaded).
const char *z_tr_at(const char **cache, const char *s);
const char *z_tr_at_small(const char **cache, const char *s);
const char *z_tr_at_l9(const char **cache, const char *s);
const char *z_tr_at_c(const char **cache, const char *ctx, const char *s);
const char *z_tr_site(const char *ctx, const char *s, const char **cache, int fit);

// True if every character of UTF-8 `s` can be drawn in z_font_5x8:
// ASCII and Latin-9 in hardware, or in the 5x8 glyphs jfont holds.
bool z_tr_fits_small(const char *s);

#endif

// -- the catalog file (tools/i18n.py writes it) --
//
// One per language, every program's translations in it, by domain (the
// program's name, and "common"). All little-endian.
//
//     offset  size  field
//     0       4     "ZLC1"
//     4       2     version (1)
//     6       1     plural rule (Z_TR_PLURAL_*)
//     7       1     0
//     8       8     language code, NUL-padded
//     16      4     number of domains
//     20      4     0
//     24      12*n  domains, by name hash ascending: name hash, first
//                   entry, number of entries
//     ...     8*m   entries, each domain's by key hash ascending: key
//                   hash, offset of the translation from the file's start
//     ...           the translations, UTF-8, NUL-terminated
//
// A key is hashed with z_tr_hash() (FNV-1a, 32 bits): the English for
// Z_(); context, 0x04, English for Z_C(); singular, 0x00, plural for
// Z_N(), whose translation is a count byte and that many NUL-terminated
// forms. tools/i18n.py refuses two keys of one program (and "common")
// with the same hash, so a lookup never compares the English.

#define Z_TR_MAGIC        "ZLC1"

#define Z_TR_PLURAL_NONE  0	// one form (Japanese)
#define Z_TR_PLURAL_ONE   1	// n != 1 (English, German, Dutch, Swedish, Italian, Spanish, Portuguese)
#define Z_TR_PLURAL_FR    2	// n > 1 (French)
#define Z_TR_PLURAL_EAST  3	// Russian, Ukrainian: 1, 21; 2-4, 22-24; the rest
#define Z_TR_PLURAL_CS    4	// Czech, Slovak: 1; 2-4; the rest
#define Z_TR_PLURAL_PL    5	// Polish: 1; 2-4, 22-24 (not 12-14); the rest

static inline uint32_t z_tr_hash2(const char *a, int sep, const char *b) {
	uint32_t h = 2166136261u;
	for (const unsigned char *p = (const unsigned char *)a; *p; p++) { h ^= *p; h *= 16777619u; }
	if (sep >= 0) {
		h ^= (uint32_t)sep; h *= 16777619u;
		for (const unsigned char *p = (const unsigned char *)b; *p; p++) { h ^= *p; h *= 16777619u; }
	}
	return h;
}

static inline uint32_t z_tr_hash(const char *s) { return z_tr_hash2(s, -1, 0); }

// The plural form for n under `rule` (0-based).
static inline int z_tr_plural(int rule, long n) {
	if (n < 0) n = -n;
	long m10 = n % 10, m100 = n % 100;
	switch (rule) {
	case Z_TR_PLURAL_NONE: return 0;
	case Z_TR_PLURAL_FR:   return n > 1;
	case Z_TR_PLURAL_EAST:
		return m10 == 1 && m100 != 11 ? 0 :
			m10 >= 2 && m10 <= 4 && (m100 < 10 || m100 >= 20) ? 1 : 2;
	case Z_TR_PLURAL_CS:   return n == 1 ? 0 : n >= 2 && n <= 4 ? 1 : 2;
	case Z_TR_PLURAL_PL:
		return n == 1 ? 0 :
			m10 >= 2 && m10 <= 4 && (m100 < 10 || m100 >= 20) ? 1 : 2;
	default:               return n != 1;
	}
}

// The translation of key `hash` in catalog `cat` (len bytes) for domain
// `dom` (its name's hash), or NULL. Pure, for the host tests.
const char *z_tr_find(const uint8_t *cat, uint32_t len, uint32_t dom, uint32_t hash);

#ifndef Z_TR_ENGLISH_ONLY
// For the host tests and the simulator: a catalog handed over directly,
// as z_jfont_use() hands over the Japanese font; and the language.
void z_tr_use(const uint8_t *cat, uint32_t len, const char *lang, const char *app);
#endif

#endif
