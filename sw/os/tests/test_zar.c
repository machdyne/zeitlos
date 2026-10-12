/*
 * Host test for sw/os/zar.c: the archive tools/mkzar.py writes, read the
 * way the kernel reads it -- apps, files, the underlay's lookups and
 * directory listings.
 *
 *   python3 tools/mkzar.py /tmp/t.zar wm=sw/apps/wm/wm.bin \
 *       net=README.md docs/welcome.txt=docs/welcome.md \
 *       docs/repl.txt=docs/boot.md help/a/b.txt=LICENSE.md
 *   cc -std=gnu99 -Wall -no-pie -DZAR_HOST_TEST -I sw/os -o /tmp/test_zar \
 *       sw/os/tests/test_zar.c sw/os/zar.c sw/common/zexec_lz4.c \
 *       && /tmp/test_zar /tmp/t.zar
 *
 * Run from the top of the tree: it compares wm as loaded (unpacked: mkzar
 * stores apps compressed) with sw/apps/wm/wm.bin. -no-pie: the loader
 * takes the process's memory as a 32-bit address, so the buffer it
 * loads into must have one.
 *
 * (The run line in docs/flash_apps.md, "Files in flash", builds the
 * inputs. `net` here is any file: the test only needs a second app.)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "zar.h"

const uint8_t *zar_test_base;
bool k_flash_session_active(void) { return false; }

// kfs.c's, without the turn-taking
static z_exec_lz4_t st;
int k_exec_unpack(uint8_t *out, uint32_t len, z_exec_fill_t fill, void *ctx) {
	return z_exec_lz4(&st, out, len, fill, ctx) ? 1 : 0;
}

static int checks, fails;
static void ok(int c, const char *what) {
	checks++;
	if (!c) { fails++; printf("FAIL %s\n", what); }
}

// The children of `dir`, as "name/" for a directory, joined by spaces.
static void kids(const char *dir, char *out) {
	char name[Z_ZAR_NAME_MAX + 1];
	uint32_t it = 0, size;
	bool d;
	out[0] = 0;
	while (z_zar_child(dir, &it, name, &size, &d)) {
		if (out[0]) strcat(out, " ");
		strcat(out, name);
		if (d) strcat(out, "/");
	}
}

int main(int argc, char **argv) {
	static uint8_t img[1 << 20];
	char buf[512], s[512];
	uint32_t off, size;
	FILE *f;
	z_exec_info_t xi;

	if (argc < 2 || !(f = fopen(argv[1], "rb"))) { printf("usage: test_zar ARCHIVE\n"); return 2; }
	size_t n = fread(img, 1, sizeof(img), f);
	fclose(f);
	zar_test_base = img;

	ok(z_zar_present() && z_zar_count() == 5, "present, five entries");
	ok(z_zar_name(0, buf) && !strcmp(buf, "wm") && !z_zar_is_file(0), "entry 0 is the app wm");
	ok(z_zar_name(2, buf) && !strcmp(buf, "docs/welcome.txt") && z_zar_is_file(2), "entry 2 is a file");

	ok(z_zar_exec_info("wm", &xi) == 0, "wm runs");
	f = fopen("sw/apps/wm/wm.bin", "rb");
	if (f) {
		static uint8_t bin[1 << 18], mem[1 << 18];
		size_t bn = fread(bin, 1, sizeof(bin), f);
		fclose(f);
		uint32_t bss = (uint32_t)bin[8] | (uint32_t)bin[9] << 8 | (uint32_t)bin[10] << 16 |
			(uint32_t)bin[11] << 24;
		ok(xi.compressed && xi.stored_size < bn - 16, "wm is stored compressed");
		ok(xi.data_size == bn - 16 && xi.bss_size == bss && xi.total == bn - 16 + bss,
			"... its sizes are the unpacked image's");
		memset(mem, 0xAA, sizeof(mem));
		if ((uintptr_t)mem > 0xFFFFFFFFu) { printf("build with -no-pie\n"); return 2; }
		ok(z_zar_load_exec((uint32_t)(uintptr_t)mem, "wm", &xi) == 0, "... it loads");
		int same = !memcmp(mem, bin + 16, bn - 16), zero = 1;
		for (uint32_t i = 0; i < bss; i++) if (mem[bn - 16 + i]) zero = 0;
		ok(same && zero, "... as wm.bin's image, then zeroed .bss");
		ok(mem[bn - 16 + bss] == 0xAA, "... and nothing past it");
		// a copy that does not unpack to its size: refused, not run (LZ4
		// carries no checksum: a flipped literal is not caught, a broken
		// structure is)
		uint32_t woff, wsize;
		z_zar_file("/apps/wm", &woff, &wsize);
		xi.data_size++;
		ok(z_zar_load_exec((uint32_t)(uintptr_t)mem, "wm", &xi) != 0, "a stream short of its size does not load");
		xi.data_size -= 2;
		ok(z_zar_load_exec((uint32_t)(uintptr_t)mem, "wm", &xi) != 0, "nor one longer than its size");
		xi.data_size++;
		img[woff + 16] = 0xFF;		// the first token: a literal run past everything
		ok(z_zar_load_exec((uint32_t)(uintptr_t)mem, "wm", &xi) != 0, "nor a broken one");
	} else {
		printf("(no sw/apps/wm/wm.bin here: the load is not checked)\n");
	}
	ok(z_zar_exec_info("net", &xi) == 0 && !xi.compressed, "a file that is no ZEXE is stored as it is");
	ok(z_zar_exec_info("docs/welcome.txt", &xi) != 0, "a file does not run");
	ok(z_zar_exec_info("WM", &xi) != 0, "app names are exact, as before");

	ok(z_zar_file("/docs/welcome.txt", &off, &size) == 0, "/docs/welcome.txt is there");
	f = fopen("docs/welcome.md", "rb");
	if (f) {
		static uint8_t want[1 << 16], got[1 << 16];
		size_t wn = fread(want, 1, sizeof(want), f);
		fclose(f);
		ok(size == wn, "... its size");
		z_zar_read(off, got, size);
		ok(!memcmp(got, want, wn), "... its bytes");
		ok((off & 3) == 0, "... 4-byte aligned");
	}
	ok(z_zar_file("/DOCS/Welcome.TXT", &off, &size) == 0, "paths match without regard to case");
	ok(z_zar_file("docs/welcome.txt", &off, &size) == 0, "with or without the leading slash");
	ok(z_zar_file("/docs/welcome", &off, &size) != 0, "a prefix is not the file");
	ok(z_zar_file("/docs/welcome.txtx", &off, &size) != 0, "nor is a longer name");
	ok(z_zar_file("/docs", &off, &size) != 0, "a directory is not a file");
	ok(z_zar_file("/apps/wm", &off, &size) == 0 && size > 16, "an app is the file /apps/<name>");
	ok(z_zar_file("/apps/w", &off, &size) != 0, "... exactly");
	ok(z_zar_file("/apps", &off, &size) != 0, "... /apps is a directory");
	ok(z_zar_file("/help/a/b.txt", &off, &size) == 0, "a file two directories down");

	kids("/", s);
	ok(!strcmp(s, "apps/ docs/ help/"), "/ holds apps/, docs/ and help/, once each");
	if (strcmp(s, "apps/ docs/ help/")) printf("     got [%s]\n", s);
	kids("/apps", s);
	ok(!strcmp(s, "wm net"), "/apps lists the apps");
	if (strcmp(s, "wm net")) printf("     got [%s]\n", s);
	kids("/APPS/", s);
	ok(!strcmp(s, "wm net"), "... any case, a trailing slash");
	kids("/docs", s);
	ok(!strcmp(s, "welcome.txt repl.txt"), "/docs lists its files");
	if (strcmp(s, "welcome.txt repl.txt")) printf("     got [%s]\n", s);
	kids("/help", s);
	ok(!strcmp(s, "a/"), "/help holds a directory");
	kids("/help/a", s);
	ok(!strcmp(s, "b.txt"), "/help/a holds b.txt");
	kids("/doc", s);
	ok(!s[0], "/doc is nothing (a prefix of docs)");
	kids("/ap", s);
	ok(!s[0], "/ap is nothing (a prefix of apps)");
	kids("/nothing", s);
	ok(!s[0], "/nothing is nothing");

	ok(z_zar_is_dir("/") && z_zar_is_dir("/docs") && z_zar_is_dir("/apps") &&
	   z_zar_is_dir("/help/a"), "the directories are directories");
	ok(!z_zar_is_dir("/docs/welcome.txt") && !z_zar_is_dir("/doc"), "... and nothing else");

	// a ZAR1 archive is not this one: nothing in it
	img[3] = '1';
	ok(!z_zar_present() && z_zar_count() == 0, "a ZAR1 archive is not read");
	(void)n;

	printf("test_zar: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
