/*
 * Host tests for sw/common/zkbd.c -- keysyms and the Z_WM_KEY packing.
 *
 *   cc -std=gnu99 -Wall -I sw/common -o /tmp/test_zkbd \
 *      sw/common/tests/test_zkbd.c sw/common/zkbd.c
 *   /tmp/test_zkbd
 *
 * See docs/keyboard_layouts.md.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "zkbd.h"
#include "zwm.h"
#include "zutf8.h"

static int run, failed;

#define CHECK(cond, what) do { run++; if (!(cond)) { failed++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, what); } } while (0)

#define SHIFT  Z_KBD_MOD_LSHIFT
#define CTRL   Z_KBD_MOD_LCTRL
#define ALTGR  Z_KBD_MOD_RALT
#define CAPS   Z_KBD_LOCK_CAPS

static int L(const char *name) {
	int id = z_kbd_layout_find(name);
	if (id < 0) { printf("FAIL: no layout %s\n", name); failed++; return 0; }
	return id;
}

static uint32_t T(const char *layout, uint8_t usage, uint8_t mods, uint8_t locks) {
	return z_kbd_translate(L(layout), usage, mods, locks, NULL);
}

// -- the keysym ranges --

static void test_ranges(void) {

	// Every named key is above Unicode, and every one fits Z_WM_KEY.
	const uint32_t named[] = {
		Z_KEY_UP, Z_KEY_DOWN, Z_KEY_LEFT, Z_KEY_RIGHT, Z_KEY_HOME,
		Z_KEY_END, Z_KEY_PAGEUP, Z_KEY_PAGEDOWN, Z_KEY_INSERT,
		Z_KEY_DELETE, Z_KEY_F1, Z_KEY_F2, Z_KEY_F3, Z_KEY_F4, Z_KEY_F5,
		Z_KEY_F6, Z_KEY_F7, Z_KEY_F8, Z_KEY_F9, Z_KEY_F10, Z_KEY_F11,
		Z_KEY_F12,
	};
	for (unsigned i = 0; i < sizeof(named) / sizeof(named[0]); i++) {
		CHECK(named[i] > 0x10FFFF, "named key inside Unicode");
		CHECK(named[i] <= Z_KEY_MAX, "named key does not fit Z_WM_KEY");
		CHECK(Z_KEY_IS_NAMED(named[i]), "Z_KEY_IS_NAMED false for a named key");
		CHECK(!Z_KEY_IS_TEXT(named[i]), "named key counted as text");
		for (unsigned j = 0; j < i; j++)
			CHECK(named[i] != named[j], "two named keys share a value");
	}

	// Characters.
	CHECK(Z_KEY_IS_TEXT('a'), "'a' not text");
	CHECK(Z_KEY_IS_TEXT(' '), "space not text");
	CHECK(Z_KEY_IS_TEXT(0xE4), "a-umlaut not text");
	CHECK(Z_KEY_IS_TEXT(0x20AC), "euro not text");
	CHECK(Z_KEY_IS_TEXT(0x3042), "hiragana a not text");
	CHECK(Z_KEY_IS_TEXT(0x10FFFD), "last plane not text");
	CHECK(!Z_KEY_IS_TEXT(0x0d), "Enter counted as text");
	CHECK(!Z_KEY_IS_TEXT(0x7f), "DEL counted as text");
	CHECK(!Z_KEY_IS_TEXT(0x85), "C1 counted as text");
	CHECK(!Z_KEY_IS_TEXT(0xD800), "surrogate counted as text");
	CHECK(!Z_KEY_IS_NAMED(0x10FFFF), "last codepoint counted as named");

}

// -- Z_WM_KEY packing --

static void test_packing(void) {

	const uint32_t syms[] = { 0, 'a', 0x7f, 0xE4, 0x20AC, 0x7FFF, 0x8000,
		0x3042, 0x10FFFF, Z_KEY_UP, Z_KEY_F12, Z_KEY_MAX };
	const uint8_t mods[] = { 0, SHIFT, CTRL | SHIFT, 0xFF };

	for (unsigned i = 0; i < sizeof(syms) / sizeof(syms[0]); i++)
		for (unsigned m = 0; m < sizeof(mods); m++)
			for (int p = 0; p < 2; p++) {
				uint32_t v = Z_WM_PACK_KEY(syms[i], mods[m], p);
				CHECK(Z_WM_UNPACK_KEY_KEYSYM(v) == syms[i], "keysym does not round-trip");
				CHECK(Z_WM_UNPACK_KEY_MODIFIERS(v) == mods[m], "modifiers do not round-trip");
				CHECK(Z_WM_UNPACK_KEY_PRESSED(v) == (uint32_t)p, "pressed does not round-trip");
			}

	// Bits 23:0 are laid out exactly as the old 15-bit packing did, so
	// an ASCII key packs to the same word it always has.
	uint32_t old = ((((uint32_t)'q') & 0x7FFF) << 9) | ((uint32_t)SHIFT << 1) | 1u;
	CHECK(Z_WM_PACK_KEY('q', SHIFT, 1) == old, "ASCII packs differently than before");

}

// -- raw events --

static void test_events(void) {

	// sw/os/hid.c's HID_EVENT() layout: bit 0 pressed, 8:1 usage,
	// 16:9 modifiers, 19:17 locks.
	uint32_t ev = (0x02u << 17) | ((uint32_t)SHIFT << 9) | (0x04u << 1) | 1u;
	CHECK(Z_KBD_EV_PRESSED(ev) == 1, "pressed bit");
	CHECK(Z_KBD_EV_USAGE(ev) == 0x04, "usage field");
	CHECK(Z_KBD_EV_MODS(ev) == SHIFT, "modifier field");
	CHECK(Z_KBD_EV_LOCKS(ev) == 0x02, "lock field");

}

// -- US translation: what it has always produced --

static void test_us(void) {

	CHECK(z_kbd_usage_to_keysym(0x04, 0) == 'a', "a");
	CHECK(z_kbd_usage_to_keysym(0x04, SHIFT) == 'A', "A");
	CHECK(z_kbd_usage_to_keysym(0x04, CTRL) == 0x01, "Ctrl+A");
	CHECK(z_kbd_usage_to_keysym(0x1D, 0) == 'z', "z");
	CHECK(z_kbd_usage_to_keysym(0x1E, 0) == '1', "1");
	CHECK(z_kbd_usage_to_keysym(0x1E, SHIFT) == '!', "!");
	CHECK(z_kbd_usage_to_keysym(0x1F, SHIFT) == '@', "@");
	CHECK(z_kbd_usage_to_keysym(0x34, SHIFT) == '"', "\"");
	CHECK(z_kbd_usage_to_keysym(0x31, 0) == '\\', "backslash");
	CHECK(z_kbd_usage_to_keysym(0x28, 0) == 0x0d, "Enter");
	CHECK(z_kbd_usage_to_keysym(0x29, 0) == 0x1b, "Escape");
	CHECK(z_kbd_usage_to_keysym(0x2A, 0) == 0x7f, "Backspace");
	CHECK(z_kbd_usage_to_keysym(0x2B, 0) == '\t', "Tab");
	CHECK(z_kbd_usage_to_keysym(0x2C, 0) == ' ', "Space");
	CHECK(z_kbd_usage_to_keysym(0x3A, 0) == Z_KEY_F1, "F1");
	CHECK(z_kbd_usage_to_keysym(0x45, 0) == Z_KEY_F12, "F12");
	CHECK(z_kbd_usage_to_keysym(0x52, 0) == Z_KEY_UP, "Up");
	CHECK(z_kbd_usage_to_keysym(0x4C, 0) == Z_KEY_DELETE, "Delete");
	CHECK(z_kbd_usage_to_keysym(0xE1, SHIFT) == Z_KEY_NONE, "bare Shift");

}

// -- layouts --

static void test_layouts(void) {

	uint8_t m;

	CHECK(z_kbd_layout_count >= 19, "layout count");
	CHECK(z_kbd_layout_count <= Z_KBD_LAYOUT_MAX, "more layouts than the event has bits for");
	CHECK(z_kbd_layout_find("us") == 0, "us is layout 0");
	CHECK(z_kbd_layout_find("DE") == z_kbd_layout_find("de"), "names are case-insensitive");
	CHECK(z_kbd_layout_find("xx") == -1, "unknown layout");
	CHECK(z_kbd_layout_info(99) == &z_kbd_layouts[0], "out of range is US");

	for (int i = 0; i < z_kbd_layout_count; i++) {
		const z_kbd_layout_t *l = &z_kbd_layouts[i];
		for (int k = 1; k < l->nkeys; k++)
			CHECK(l->keys[k - 1].usage < l->keys[k].usage, "keys not sorted");
		CHECK(l->label && l->label[0] && l->label[1] && !l->label[2], "label is two letters");
		// Every layout types every ASCII letter somewhere, plain.
		for (char c = 'a'; c <= 'z'; c++) {
			bool found = false;
			for (int k = 0; k < l->nkeys; k++)
				if (l->keys[k].sym[0] == (uint16_t)c) found = true;
			CHECK(found, "a layout cannot type a letter");
		}
	}

	// US: right Alt is Alt, not AltGr, and passes through untouched.
	CHECK(z_kbd_translate(0, 0x14, ALTGR, 0, &m) == 'q' && m == ALTGR, "US right Alt");

	// German.
	CHECK(T("de", 0x1C, 0, 0) == 'z', "de: Z where US has Y");
	CHECK(T("de", 0x1D, 0, 0) == 'y', "de: Y where US has Z");
	CHECK(T("de", 0x34, 0, 0) == 0xE4, "de: a-umlaut");
	CHECK(T("de", 0x34, SHIFT, 0) == 0xC4, "de: A-umlaut");
	CHECK(T("de", 0x34, 0, CAPS) == 0xC4, "de: Caps Lock capitalises a-umlaut");
	CHECK(T("de", 0x34, SHIFT, CAPS) == 0xE4, "de: Shift undoes Caps Lock");
	CHECK(T("de", 0x2D, 0, 0) == 0xDF, "de: sharp s");
	CHECK(T("de", 0x1F, SHIFT, 0) == '"', "de: Shift+2");
	CHECK(T("de", 0x1E, 0, CAPS) == '1', "de: Caps Lock leaves digits");
	CHECK(z_kbd_translate(L("de"), 0x14, ALTGR, 0, &m) == '@' && m == 0, "de: AltGr+Q is @, AltGr bit consumed");
	CHECK(T("de", 0x08, ALTGR, 0) == 0x20AC, "de: AltGr+E is the euro");
	CHECK(T("de", 0x25, ALTGR, 0) == '[', "de: AltGr+8 is [");
	CHECK(T("de", 0x64, 0, 0) == '<', "de: ISO key");
	CHECK(T("de", 0x64, ALTGR, 0) == '|', "de: AltGr+ISO key");
	CHECK(T("de", 0x1C, CTRL, 0) == 0x1A, "de: Ctrl+Z follows the layout");
	CHECK(T("de", 0x63, 0, 0) == ',', "de: keypad decimal is a comma");
	CHECK(Z_KEY_IS_DEAD(T("de", 0x2E, 0, 0)), "de: acute is dead");
	CHECK(Z_KEY_IS_DEAD(T("de", 0x35, 0, 0)), "de: circumflex is dead");
	CHECK(T("de-nodeadkeys", 0x35, 0, 0) == '^', "de-nodeadkeys: circumflex types");
	CHECK(T("de-nodeadkeys", 0x2E, 0, 0) == 0xB4, "de-nodeadkeys: acute types");

	// A key with nothing on its AltGr levels types its plain character
	// and keeps right Alt.
	CHECK(z_kbd_translate(L("de"), 0x1C, 0, 0, &m) == 'z', "de: z");

	// UK.
	CHECK(T("gb", 0x1F, SHIFT, 0) == '"', "gb: Shift+2");
	CHECK(T("gb", 0x20, SHIFT, 0) == 0xA3, "gb: pound");
	CHECK(T("gb", 0x32, 0, 0) == '#', "gb: ISO hash key");
	CHECK(T("gb", 0x64, 0, 0) == '\\', "gb: ISO backslash key");

	// French AZERTY.
	CHECK(T("fr", 0x14, 0, 0) == 'a', "fr: A where US has Q");
	CHECK(T("fr", 0x04, 0, 0) == 'q', "fr: Q where US has A");
	CHECK(T("fr", 0x33, 0, 0) == 'm', "fr: M right of L");
	CHECK(T("fr", 0x1F, 0, 0) == 0xE9, "fr: e-acute on 2");
	CHECK(T("fr", 0x1E, SHIFT, 0) == '1', "fr: digits need Shift");
	CHECK(T("fr", 0x27, ALTGR, 0) == '@', "fr: AltGr+0 is @");
	CHECK(T("fr", 0x14, CTRL, 0) == 0x01, "fr: Ctrl+A follows the layout");

	// Spanish, Latin American, Italian, Brazilian.
	CHECK(T("es", 0x33, 0, 0) == 0xF1, "es: n-tilde");
	CHECK(T("es", 0x33, 0, CAPS) == 0xD1, "es: Caps Lock N-tilde");
	CHECK(T("latam", 0x33, 0, 0) == 0xF1, "latam: n-tilde");
	CHECK(T("it", 0x2F, 0, 0) == 0xE8, "it: e-grave");
	CHECK(T("br", 0x33, 0, 0) == 0xE7, "br: c-cedilla");
	CHECK(T("br", 0x87, 0, 0) == '/', "br: the ABNT2 key");
	CHECK(T("br", 0x63, 0, 0) == ',', "br: keypad decimal is a comma");

	// Phase 5: the second wave.
	CHECK(T("ch", 0x1C, 0, 0) == 'z', "ch: QWERTZ");
	CHECK(T("ch", 0x2F, 0, 0) == 0xFC, "ch: u-umlaut");
	CHECK(T("ch", 0x2F, SHIFT, 0) == 0xE8, "ch: e-grave on Shift");
	CHECK(T("ch-fr", 0x2F, 0, 0) == 0xE8, "ch-fr: e-grave plain");
	CHECK(T("se", 0x2F, 0, 0) == 0xE5, "se: a-ring");
	CHECK(T("se", 0x33, 0, 0) == 0xF6, "se: o-umlaut");
	CHECK(T("fi", 0x34, 0, 0) == 0xE4, "fi: a-umlaut");
	CHECK(T("dk", 0x33, 0, 0) == 0xE6, "dk: ae");
	CHECK(T("dk", 0x34, 0, 0) == 0xF8, "dk: o-slash");
	CHECK(T("no", 0x33, 0, 0) == 0xF8, "no: o-slash");
	CHECK(T("no", 0x34, 0, 0) == 0xE6, "no: ae");
	CHECK(T("no", 0x33, 0, CAPS) == 0xD8, "no: Caps Lock O-slash");
	CHECK(T("pt", 0x33, 0, 0) == 0xE7, "pt: c-cedilla");
	CHECK(T("be", 0x14, 0, 0) == 'a', "be: AZERTY");
	CHECK(Z_KEY_IS_DEAD(T("us-intl", 0x34, 0, 0)), "us-intl: apostrophe is a dead acute");
	CHECK(z_kbd_compose(T("us-intl", 0x34, 0, 0), 'e') == 0xE9, "us-intl: ' e is e-acute");
	CHECK(T("us-intl", 0x06, ALTGR, 0) == 0xA9, "us-intl: AltGr+C is copyright");
	CHECK(T("jp", 0x2F, 0, 0) == '@', "jp: @ right of P");
	CHECK(T("jp", 0x1F, SHIFT, 0) == '"', "jp: Shift+2");
	// xkb, like every Linux desktop, types backslash on the yen key:
	// Japanese fonts traditionally draw 0x5C as the yen sign.
	CHECK(T("jp", 0x89, 0, 0) == 0x5C, "jp: the yen key types backslash, as xkb does");
	CHECK(T("jp", 0x87, SHIFT, 0) == '_', "jp: the ro key");

	// Keys every layout shares, and the keypad (all layouts).
	for (int i = 0; i < z_kbd_layout_count; i++) {
		CHECK(z_kbd_translate(i, 0x28, 0, 0, NULL) == 0x0d, "Enter");
		CHECK(z_kbd_translate(i, 0x52, 0, 0, NULL) == Z_KEY_UP, "Up");
		CHECK(z_kbd_translate(i, 0x59, 0, 0, NULL) == '1', "keypad 1");
		CHECK(z_kbd_translate(i, 0x62, 0, 0, NULL) == '0', "keypad 0");
		CHECK(z_kbd_translate(i, 0x58, 0, 0, NULL) == 0x0d, "keypad Enter");
		CHECK(z_kbd_translate(i, 0xE6, ALTGR, 0, NULL) == Z_KEY_NONE, "bare AltGr");
	}
	CHECK(T("us", 0x63, 0, 0) == '.', "us: keypad decimal is a stop");

	// The layout travels in the event.
	int32_t ev = (int32_t)(((uint32_t)L("de") << 20) | (0x34u << 1) | 1u);
	CHECK(Z_KBD_EV_LAYOUT(ev) == (uint32_t)L("de"), "layout field");
	CHECK(z_kbd_event_to_keysym(ev, NULL) == 0xE4, "event in de");
	CHECK(z_kbd_event_to_keysym(ev & ~(0x1F << 20), NULL) == '\'', "same key in us");

}

// -- dead keys --

static void test_dead(void) {

	uint32_t acute = T("de", 0x2E, 0, 0);
	uint32_t grave = T("de", 0x2E, SHIFT, 0);
	uint32_t circ  = T("de", 0x35, 0, 0);
	uint32_t diaer = T("fr", 0x2F, SHIFT, 0);

	CHECK(z_kbd_compose(acute, 'e') == 0xE9, "acute e");
	CHECK(z_kbd_compose(acute, 'E') == 0xC9, "acute E");
	CHECK(z_kbd_compose(grave, 'a') == 0xE0, "grave a");
	CHECK(z_kbd_compose(circ, 'o') == 0xF4, "circumflex o");
	CHECK(z_kbd_compose(diaer, 'y') == 0xFF, "diaeresis y");
	CHECK(z_kbd_compose(diaer, 'Y') == 0x178, "diaeresis Y (Latin-9)");
	CHECK(z_kbd_compose(acute, 'q') == 0, "acute q does not combine");
	CHECK(z_kbd_compose('a', 'e') == 0, "not a dead key");

	CHECK(z_kbd_dead_spacing_of(circ) == '^', "circumflex alone");
	CHECK(z_kbd_dead_spacing_of(grave) == '`', "grave alone");
	CHECK(z_kbd_dead_spacing_of(acute) == 0xB4, "acute alone");

	// Every composition in the table is findable (the search is right).
	for (int i = 0; i < z_kbd_compose_count; i++) {
		const z_kbd_compose_t *e = &z_kbd_compose_table[i];
		uint32_t d = Z_KEY_DEAD(e->dead - Z_KBD_DEAD_BASE);
		CHECK(z_kbd_compose(d, e->base) == e->result, "compose table entry");
	}

}

// -- romaji to kana --

// Types `romaji` through the converter, then a flush, and returns the
// result as UTF-8 in a static buffer.
static const char *ime(const char *romaji, int mode) {
	static char out[128];
	z_kbd_ime_t st;
	memset(&st, 0, sizeof(st));
	st.mode = (uint8_t)mode;
	uint32_t cps[Z_KBD_IME_OUT];
	int o = 0;
	for (const char *p = romaji; *p; p++) {
		if (!z_kbd_ime_takes((uint8_t)*p)) {
			int n = z_kbd_ime_flush(&st, cps);
			for (int i = 0; i < n; i++) o += z_utf8_put(cps[i], out + o);
			o += z_utf8_put((uint8_t)*p, out + o);
			continue;
		}
		int n = z_kbd_ime_feed(&st, (uint8_t)*p, cps);
		for (int i = 0; i < n; i++) o += z_utf8_put(cps[i], out + o);
	}
	int n = z_kbd_ime_flush(&st, cps);
	for (int i = 0; i < n; i++) o += z_utf8_put(cps[i], out + o);
	out[o] = 0;
	return out;
}

#define HIRA Z_KBD_IME_HIRAGANA
#define KATA Z_KBD_IME_KATAKANA
#define IME_IS(r, m, want) CHECK(!strcmp(ime(r, m), want), r " -> " want)

static void test_ime(void) {

	IME_IS("aiueo", HIRA, "あいうえお");
	IME_IS("konnichiha", HIRA, "こんにちは");
	IME_IS("konnnichiha", HIRA, "こんにちは");		// three n's, as some type it
	IME_IS("konna", HIRA, "こんな");				// nn before a vowel
	IME_IS("kanji", HIRA, "かんじ");				// n before a consonant
	IME_IS("hon", HIRA, "ほん");					// n at the end
	IME_IS("hon'ya", HIRA, "ほんや");				// n' splits
	IME_IS("honya", HIRA, "ほにゃ");				// without it, nya
	IME_IS("gakkou", HIRA, "がっこう");			// doubled consonant
	IME_IS("matte", HIRA, "まって");
	IME_IS("shinbun", HIRA, "しんぶん");
	IME_IS("sinbun", HIRA, "しんぶん");			// kunrei
	IME_IS("tsukue", HIRA, "つくえ");
	IME_IS("tukue", HIRA, "つくえ");
	IME_IS("kyouto", HIRA, "きょうと");
	IME_IS("chotto", HIRA, "ちょっと");
	IME_IS("jiyuu", HIRA, "じゆう");
	IME_IS("zya", HIRA, "じゃ");
	IME_IS("fairu", HIRA, "ふぁいる");
	IME_IS("xtu", HIRA, "っ");
	IME_IS("ltu", HIRA, "っ");
	IME_IS("nihongo.", HIRA, "にほんご。");		// n flushed by punctuation
	IME_IS("ra-men", HIRA, "らーめん");
	IME_IS("[a],", HIRA, "「あ」、");
	IME_IS("KONNICHIHA", HIRA, "こんにちは");		// case does not matter
	IME_IS("ko-hi-", KATA, "コーヒー");
	IME_IS("zeitorosu", KATA, "ゼイトロス");
	IME_IS("konpyu-ta-", KATA, "コンピューター");
	IME_IS("don't", HIRA, "どんt");				// n' is the syllabic n, even here
	IME_IS("a'i", HIRA, "あ'い");				// not after n, an apostrophe is itself
	IME_IS("vaiorin", KATA, "ヴァイオリン");
	IME_IS("qwe", HIRA, "qうぇ");				// q is nothing: itself
	IME_IS("a 1", HIRA, "あ 1");				// space and digits pass through

	// Backspace takes back a pending letter.
	z_kbd_ime_t st;
	memset(&st, 0, sizeof(st));
	st.mode = HIRA;
	uint32_t o[Z_KBD_IME_OUT];
	CHECK(z_kbd_ime_feed(&st, 'k', o) == 0, "k waits");
	CHECK(z_kbd_ime_backspace(&st), "Backspace takes it back");
	CHECK(!z_kbd_ime_backspace(&st), "then nothing is pending");
	CHECK(z_kbd_ime_feed(&st, 'a', o) == 1 && o[0] == 0x3042, "a is あ");

	// The input method layouts.
	CHECK(z_kbd_layout_info(z_kbd_layout_find("ja"))->ime == Z_KBD_IME_HIRAGANA, "ja is hiragana");
	CHECK(z_kbd_layout_info(z_kbd_layout_find("ja-kata"))->ime == Z_KBD_IME_KATAKANA, "ja-kata");
	CHECK(z_kbd_layout_info(z_kbd_layout_find("ja"))->keys ==
		z_kbd_layout_info(z_kbd_layout_find("jp"))->keys, "ja shares jp's key table");
	CHECK(z_kbd_layout_info(z_kbd_layout_find("us"))->ime == Z_KBD_IME_NONE, "us is not");

}

int main(void) {

	test_ime();
	test_ranges();
	test_packing();
	test_events();
	test_us();
	test_layouts();
	test_dead();

	printf("%d checks, %d failed\n", run, failed);
	return failed ? 1 : 0;

}
