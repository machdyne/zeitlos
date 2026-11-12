/*
 * jfont -- the Japanese font, other scripts' glyphs and the
 * translations, held once for every app
 *
 *   > run jfont
 *
 * wm starts it at boot when zeitlos.cfg asks for Japanese
 * (system.font.japanese: yes) or for a language other than English
 * (system.language: de). It loads, from the card's "lang" set, into its
 * arena:
 *
 *   - /data/jfont/lang/<lang>.zlc   the language's translations
 *                                   (tools/i18n.py), unless English;
 *   - /data/jfont/script5x8.zfn,    the glyphs the hardware fonts lack:
 *     script6x12.zfn                Latin Extended-A, Greek, Cyrillic
 *                                   (tools/gen_script_fonts.py), 12KB;
 *   - /data/jfont/jp12.zfn          the Japanese font, 175KB -- only for
 *                                   system.language: ja, or
 *                                   system.font.japanese: yes;
 *
 * then writes the directory in sw/common/zlang.h, which apps find with
 * no message and read straight from: every byte has one physical
 * address any process may read (docs/mpu.md). It answers no messages,
 * and runs until the machine stops: apps keep pointers into what it
 * holds. See docs/i18n.md and docs/text_encoding.md, "Japanese".
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zfsapp.h"
#include "../../common/zcfg.h"
#include "../../common/zjfont.h"
#include "../../common/zlang.h"
#include "../../common/zi18n.h"

// The directory, in this process's own block, where clients scan for it.
static z_lang_dir_t dir __attribute__((aligned(4)));

// Files are read through a buffer of our own: drivers cannot write
// into an arena (zproc.h, Z_SYS_ARENA).
static uint8_t chunk[4096];

// -- console output, without stdio --
//
// printf would pull ~40KB of newlib into a process whose whole point is
// to be small. uart_putc() is in zeitlos.c.
void uart_putc(char c);

static void say(const char *s) {
	while (*s) { if (*s == '\n') uart_putc('\r'); uart_putc(*s++); }
}

static void say_dec(uint32_t v) {
	char b[11];
	int i = 10;
	b[i] = 0;
	do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v && i);
	say(&b[i]);
}

// Physical address of something in this process -- see blit_phys_addr()
// in zgfx.c, the same translation.
static uint32_t phys(const void *p) {
	uint32_t v = (uint32_t)(uintptr_t)p;
	uint32_t base = reg_mtu_base;
	if (base == 0 || (v & 0xF0000000u) != 0x80000000u) return v;
	return base + (v & 0x0FFFFFFFu);
}

// -- what to load --

#define MAXF Z_LANG_ENTRIES

static struct {
	uint32_t	kind;
	char		path[64];
	int			size;
	const char	*magic;
} want[MAXF];
static int nwant;

static void add(uint32_t kind, const char *path, const char *magic, bool quiet) {
	if (nwant >= MAXF) return;
	int size = fs_size((char *)path);
	if (size < 12) {
		if (quiet) return;
		say("jfont: no ");
		say(path);
		say("\n");
		return;
	}
	want[nwant].kind = kind;
	strncpy(want[nwant].path, path, sizeof(want[nwant].path) - 1);
	want[nwant].size = size;
	want[nwant].magic = magic;
	nwant++;
}

// Reads a whole file into `dst` (arena memory). Returns bytes read.
static int load(const char *path, uint8_t *dst, int size) {
	int h = fs_open_read((char *)path);
	if (h < 0) return -1;
	int n = 0;
	while (n < size) {
		int want_n = size - n > (int)sizeof(chunk) ? (int)sizeof(chunk) : size - n;
		int r = fs_read_chunk(h, chunk, want_n);
		if (r <= 0) break;
		memcpy(dst + n, chunk, (size_t)r);
		n += r;
	}
	fs_close_handle(h);
	return n;
}

int main(void) {

	char name[24];

	// One is enough. The registry numbers names in order, so anything
	// but "jfont0" means one is already running.
	if (!z_pid_register(Z_JFONT_PROC, name, sizeof(name)) || strcmp(name, Z_JFONT_NAME)) {
		say("jfont: already running\n");
		return 0;
	}

	char lang[16];
	if (!z_cfg_get("system.language", lang, sizeof lang) || !lang[0]) strcpy(lang, "en");
	lang[sizeof(dir.lang) - 1] = 0;
	bool japanese = !strcmp(lang, "ja") || z_cfg_get_bool("system.font.japanese", false);

	add(Z_LANG_FONT_5X8, Z_PATH_JFONT_SCRIPT5X8, "ZFN1", false);
	add(Z_LANG_FONT_6X12, Z_PATH_JFONT_SCRIPT6X12, "ZFN1", false);
	if (japanese) add(Z_LANG_FONT_JP12, Z_PATH_JFONT_FONT, "ZFN1", false);

	// The translations: the shared library's (common), then every app's
	// own, in its own folder -- /data/<app>/lang/<lang>.zlc -- so an
	// app brings its translations with it and nothing else changes when
	// one is added (docs/i18n.md). One listing of /data, then one
	// question per folder.
	if (strcmp(lang, "en")) {
		char p[64];
		strcpy(p, Z_DIR_LANG "/");
		strcat(p, lang);
		strcat(p, ".zlc");
		add(Z_LANG_CATALOG, p, Z_TR_MAGIC, false);

		static uint8_t types[128];
		uint32_t count = 0;
		if (fs_list_into(Z_DIR_DATA, (char *)chunk, sizeof chunk, types, sizeof types, &count, 0)) {
			const char *e = (const char *)chunk;
			for (uint32_t i = 0; i < count; i++, e += strlen(e) + 1) {
				if (types[i] != Z_FS_TYPE_DIR) continue;
				const char *b = strrchr(e, '/');
				b = b ? b + 1 : e;
				if (!b[0] || !strcmp(b, "jfont")) continue;
				if (strlen(Z_DIR_DATA) + strlen(b) + strlen(lang) + 12 >= sizeof p) continue;
				strcpy(p, Z_DIR_DATA "/");
				strcat(p, b);
				strcat(p, "/lang/");
				strcat(p, lang);
				strcat(p, ".zlc");
				add(Z_LANG_CATALOG, p, Z_TR_MAGIC, true);
			}
		}
	}

	uint32_t total = 0;
	for (int i = 0; i < nwant; i++) total += ((uint32_t)want[i].size + 3) & ~3u;
	if (!total) {
		say("jfont: nothing to hold\n");
		return 1;
	}

	void *base = 0;
	uint32_t got = 0;
	if (z_arena_set(total, &base, &got) != Z_ARENA_OK || !base || got < total) {
		say("jfont: no room for ");
		say_dec(total);
		say(" bytes\n");
		return 1;
	}

	// Load everything, then publish: an app never sees part of it.
	uint8_t *at = (uint8_t *)base;
	int slot = 0;
	for (int i = 0; i < nwant; i++) {
		int n = load(want[i].path, at, want[i].size);
		if (n != want[i].size || memcmp(at, want[i].magic, 4)) {
			say("jfont: ");
			say(want[i].path);
			say(" is not what it should be -- skipped\n");
			continue;
		}
		dir.entry[slot].kind = want[i].kind;
		dir.entry[slot].addr = phys(at);
		dir.entry[slot].len = (uint32_t)n;
		slot++;
		at += ((uint32_t)n + 3) & ~3u;
	}

	// The directory, magic last: until magic0 is written no client can
	// match it, so no client ever sees a half-written one.
	uint32_t where = phys(&dir);
	dir.version = 1;
	memcpy(dir.lang, lang, strlen(lang) + 1);
	dir.check = z_lang_check(where);
	dir.magic1 = Z_LANG_MAGIC1;
	dir.magic0 = Z_LANG_MAGIC0;

	say("jfont: ");
	say(lang);
	say(", ");
	say_dec((uint32_t)slot);
	say(" parts, ");
	say_dec(total);
	say(" bytes\n");

	// Nothing to do but exist. Messages are read and dropped so a stray
	// one cannot fill the mailbox.
	for (;;) {
		z_msg_t msg;
		while (z_msg_read(&msg) == Z_OK) {}
		z_proc_wait(0);
	}

}
