/*
 * Host test for `zetta FILE` (sw/apps/zetta/edit.c): real files, the
 * screen through the real terminal emulator.
 *
 *   make -C sw/apps/zetta test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "../edit.h"
#include "../../../common/zvt100.h"

static int checks, fails;
#define CK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static vt_screen_t vt;
static uint32_t now_ms = 1000;
static char dir[64];

static void wr(void *ctx, const char *b, uint32_t n) { (void)ctx; vt_feed(&vt, (const uint8_t *)b, n); }

static bool on_screen(const char *s) {
	for (int r = 0; r < VT_ROWS; r++) {
		char b[VT_COLS + 1];
		for (int c = 0; c < VT_COLS; c++) b[c] = vt.cells[r][c].ch ? vt.cells[r][c].ch : ' ';
		b[VT_COLS] = 0;
		if (strstr(b, s)) return true;
	}
	return false;
}

static bool type(const char *s) { now_ms += 20; return ze_input((const uint8_t *)s, (uint32_t)strlen(s), now_ms); }
static bool esc(void) { type("\x1b"); now_ms += 300; return ze_tick(now_ms); }

static const char *path(const char *name) {
	static char p[4][128];
	static int k;
	k = (k + 1) % 4;
	snprintf(p[k], sizeof(p[k]), "%s/%s", dir, name);
	return p[k];
}

static char *slurp(const char *p, long *len) {
	static char b[70000];
	FILE *f = fopen(p, "rb");
	if (!f) { *len = -1; return NULL; }
	*len = (long)fread(b, 1, sizeof(b) - 1, f);
	fclose(f);
	b[*len] = 0;
	return b;
}

static void put(const char *p, const char *s, long n) {
	FILE *f = fopen(p, "wb");
	fwrite(s, 1, (size_t)n, f);
	fclose(f);
}

int main(void) {
	char why[200];
	long n;
	snprintf(dir, sizeof(dir), "/tmp/zetta-test-%d", (int)getpid());
	mkdir(dir, 0755);

	// -- 1. a new file --
	vt_init(&vt);
	CK(ze_open(path("new.txt"), 25, 80, wr, NULL, why, sizeof(why)), "a file that is not there: opened, new");
	CK(on_screen("zetta -- ") && on_screen("new.txt") && on_screen("A new file."), "the title names it; the status says it is new");
	CK(on_screen("^O Save"), "Save in the help bar");
	type("Hello world\rSecond line");
	CK(!type("\x0f"), "Ctrl-O: saved, still editing");
	char *s = slurp(path("new.txt"), &n);
	CK(s && !strcmp(s, "Hello world\nSecond line"), "the file is what was typed, exactly (%s)", s ? s : "missing");
	CK(on_screen("Saved: 23 bytes"), "and the status says so");
	CK(type("\x18"), "Ctrl-X, nothing changed since: finished at once");

	// -- 2. an existing file: a long line stays one line; CRLF; a tab --
	{
		char text[400];
		int o = 0;
		for (int i = 0; i < 25; i++) o += sprintf(text + o, "word%02d ", i);	// 175 characters, one line
		o += sprintf(text + o, "end\r\n\tindented\r\nlast");
		put(path("long.txt"), text, o);
		vt_init(&vt);
		CK(ze_open(path("long.txt"), 25, 80, wr, NULL, why, sizeof(why)), "an existing file opened");
		CK(on_screen("word00 word01") && on_screen("word11 word12"), "its long line shown wrapped");
		type("\x0f");
		s = slurp(path("long.txt"), &n);
		char *nl = s ? strchr(s, '\n') : NULL;
		CK(nl && nl - s == 178 && !strcmp(nl, "\n\tindented\nlast"), "saved: the long line still one line; the tab kept; CRLF now LF");
		type("\x18");
	}

	// -- 3. leaving with changes: nano's question --
	{
		put(path("keep.txt"), "original", 8);
		vt_init(&vt);
		ze_open(path("keep.txt"), 25, 80, wr, NULL, why, sizeof(why));
		type(" changed");
		CK(!type("\x18") && on_screen("Save changes to") && on_screen("keep.txt? (y/n)"), "Ctrl-X with changes: it asks");
		CK(!esc(), "Esc: still editing");
		type("\x18");
		CK(type("n"), "n: finished");
		s = slurp(path("keep.txt"), &n);
		CK(s && !strcmp(s, "original"), "and the file untouched (%s)", s);
		vt_init(&vt);
		ze_open(path("keep.txt"), 25, 80, wr, NULL, why, sizeof(why));
		type("\x1b[F and more");
		type("\x18");
		CK(type("y"), "y: saved, and finished");
		s = slurp(path("keep.txt"), &n);
		CK(s && !strcmp(s, "original and more"), "the change saved (%s)", s);
	}

	// -- 4. no name yet: Ctrl-O asks --
	{
		vt_init(&vt);
		CK(ze_open("", 25, 80, wr, NULL, why, sizeof(why)) && on_screen("(no name yet)"), "no file named: a new one with no name");
		type("Nameless text.");
		type("\x0f");
		CK(on_screen("Save as:"), "Ctrl-O: asks for a name");
		type(path("named.txt"));
		type("\r");
		s = slurp(path("named.txt"), &n);
		CK(s && !strcmp(s, "Nameless text."), "saved under it");
		CK(on_screen("named.txt"), "the title now names it");
		type("\x18");
	}

	// -- 5. Save as, from the menu --
	{
		vt_init(&vt);
		ze_open(path("named.txt"), 25, 80, wr, NULL, why, sizeof(why));
		esc();
		CK(on_screen("Save as..."), "Esc: the menu has Save as");
		type("\x1b[B\r");
		CK(on_screen("Save as:") && on_screen("named.txt"), "the prompt, with the name as it is");
		type("\x7f\x7f\x7f" "copy");							// named.txt -> named.copy
		type("\r");
		s = slurp(path("named.copy"), &n);
		CK(s && !strcmp(s, "Nameless text."), "a copy saved under the new name");
		type("\x18");
	}

	// -- 6. what is refused, and a save that cannot happen --
	{
		static char big[ZE_TEXT_MAX];
		memset(big, 'x', sizeof(big));
		put(path("big.txt"), big, sizeof(big));
		CK(!ze_open(path("big.txt"), 25, 80, wr, NULL, why, sizeof(why)) && strstr(why, "zetta takes up to"),
			"a file too big: refused, saying why (%s)", why);
		put(path("bin.dat"), "ab\0cd", 5);
		CK(!ze_open(path("bin.dat"), 25, 80, wr, NULL, why, sizeof(why)) && strstr(why, "not text"),
			"a NUL byte: not text, refused (%s)", why);
		vt_init(&vt);
		ze_open(path("nowhere/file.txt"), 25, 80, wr, NULL, why, sizeof(why));
		type("text");
		CK(!type("\x0f") && on_screen("Cannot save there"), "saving into a directory that is not there: said, still editing");
		struct stat st;
		CK(stat(path("nowhere/file.txt.~"), &st) != 0, "and no temporary file left");
	}

	printf("zetta FILE: %d checks, %d failed\n", checks, fails);
	char cmd[128];
	snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
	if (!fails) { int r = system(cmd); (void)r; }
	return fails != 0;
}
