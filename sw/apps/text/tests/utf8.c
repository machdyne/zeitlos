/*
 * Host test for sw/apps/text's UTF-8 handling -- the real text.c,
 * driven by real keysyms, loading and saving through an in-memory
 * filesystem behind a scripted kernel.
 *
 *   cc -std=gnu99 -Wall -no-pie -I sw/common -o /tmp/text_utf8 \
 *      sw/apps/text/tests/utf8.c sw/common/zwin.c sw/common/zwidget.c \
 *      sw/common/zflist.c sw/common/zedit.c sw/common/zfsapp.c \
 *      sw/common/zfont_data.c sw/common/zobj.c sw/common/zeitlos.c \
 *      sw/common/zspeak.c sw/common/zsayall.c sw/common/zdialog.c
 *   /tmp/text_utf8
 *
 * Needs -no-pie and vm.mmap_min_addr=0, like every host test that runs
 * an app's own code (sw/common/tests/ztramp.h explains both); exits 77
 * when it cannot have them. See docs/text_editor.md, "Encodings".
 */

#include "../../../common/tests/zrender.h"

#include <stdarg.h>

#include "../../../common/zfs.h"	// the FS syscall argument blocks

#define main text_main_unused
#include "../text.c"
#undef main

// zgfx.c pieces zrender.h does not stand in for, and nothing here
// exercises: scroll diagnostics and file-list icons.
int z_fb_scroll_debug, z_fb_scroll_dbg_armed, z_fb_scroll_align;
void z_fb_draw_icon(int x, int y, int icon_id, int fg, int bg, const z_clip_t *clip) {
	(void)x; (void)y; (void)icon_id; (void)fg; (void)bg; (void)clip;
}

// ---------------------------------------------------------------
// an in-memory filesystem, behind a scripted kernel
// ---------------------------------------------------------------

#define NFILES  8
#define FILEMAX 65536

typedef struct {
	char name[64];
	unsigned char data[FILEMAX];
	int size;
	bool used;
} mfile_t;

static mfile_t files[NFILES];

// One open handle at a time is all text.c ever has.
static mfile_t *open_file;
static int open_pos;
static bool open_writing;

static mfile_t *find(const char *name, bool create) {
	for (int i = 0; i < NFILES; i++)
		if (files[i].used && !strcmp(files[i].name, name)) return &files[i];
	if (!create) return NULL;
	for (int i = 0; i < NFILES; i++)
		if (!files[i].used) {
			files[i].used = true;
			snprintf(files[i].name, sizeof(files[i].name), "%s", name);
			files[i].size = 0;
			return &files[i];
		}
	return NULL;
}

static void put_file(const char *name, const void *data, int n) {
	mfile_t *f = find(name, true);
	memcpy(f->data, data, (size_t)n);
	f->size = n;
}

static z_obj_t k_ok, k_fail;
static uint32_t ticks = 1000;

static uint32_t *k_syscall(uint32_t id, uint32_t *args, uint32_t b) {

	(void)b;

	switch (id) {

	case Z_SYS_UPTIME:
		((z_obj_t *)args)->type = Z_UINT32;
		((z_obj_t *)args)->val.uint32 = ticks++;
		return (uint32_t *)&k_ok;

	case Z_SYS_MSG_READ:
		return (uint32_t *)&k_fail;		// mailbox always empty

	case Z_SYS_FS_SIZE: {
		z_fs_size_args_t *a = (z_fs_size_args_t *)args;
		mfile_t *f = find(a->name, false);
		if (!f) return (uint32_t *)&k_fail;
		a->size = (uint32_t)f->size;
		return (uint32_t *)&k_ok;
	}

	case Z_SYS_FS_OPEN_READ:
	case Z_SYS_FS_OPEN_WRITE: {
		z_fs_open_args_t *a = (z_fs_open_args_t *)args;
		bool w = (id == Z_SYS_FS_OPEN_WRITE);
		mfile_t *f = find(a->name, w);
		if (!f || open_file) return (uint32_t *)&k_fail;
		if (w) f->size = 0;			// created or truncated
		open_file = f;
		open_pos = 0;
		open_writing = w;
		a->handle = 1;
		return (uint32_t *)&k_ok;
	}

	case Z_SYS_FS_READ_CHUNK: {
		z_fs_read_chunk_args_t *a = (z_fs_read_chunk_args_t *)args;
		if (!open_file || open_writing) return (uint32_t *)&k_fail;
		int n = open_file->size - open_pos;
		if (n > (int)a->maxlen) n = (int)a->maxlen;
		// Small reads, to exercise the loader's chunk loop.
		if (n > 1000) n = 1000;
		memcpy(a->buf, &open_file->data[open_pos], (size_t)n);
		open_pos += n;
		a->len = (uint32_t)n;
		return (uint32_t *)&k_ok;
	}

	case Z_SYS_FS_WRITE_CHUNK: {
		z_fs_write_chunk_args_t *a = (z_fs_write_chunk_args_t *)args;
		if (!open_file || !open_writing) return (uint32_t *)&k_fail;
		if (open_file->size + (int)a->len > FILEMAX) return (uint32_t *)&k_fail;
		memcpy(&open_file->data[open_file->size], a->buf, a->len);
		open_file->size += (int)a->len;
		a->written = a->len;
		return (uint32_t *)&k_ok;
	}

	case Z_SYS_FS_CLOSE:
		open_file = NULL;
		return (uint32_t *)&k_ok;

	default:
		return (uint32_t *)&k_ok;

	}

}

static bool k_install(void) {

	if ((uintptr_t)(void *)k_syscall > 0xFFFFFFFFu) return false;

	void *page = mmap((void *)0, 4096, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if (page == MAP_FAILED) return false;

	k_ok.type = Z_UINT32; k_ok.val.uint32 = Z_OK;
	k_fail.type = Z_UINT32; k_fail.val.uint32 = Z_FAIL;

	*(volatile uint32_t *)0x0000000c = (uint32_t)(uintptr_t)k_syscall;
	return true;

}

// ---------------------------------------------------------------
// checks
// ---------------------------------------------------------------

static int checks, failures;

static void expect(bool ok, const char *what) {
	checks++;
	if (!ok) { failures++; printf("  FAIL: %s\n", what); }
}

static bool doc_is(const char *s) {
	return len == (int)strlen(s) && !memcmp(buf, s, (size_t)len);
}

static bool file_is(const char *name, const void *data, int n) {
	mfile_t *f = find(name, false);
	return f && f->size == n && !memcmp(f->data, data, (size_t)n);
}

static void key(uint32_t keysym) { handle_key(keysym, 0); }
static void key_m(uint32_t keysym, uint8_t mods) { handle_key(keysym, mods); }

// Types a UTF-8 string, one keysym per character. A newline is sent
// as Enter sends it, CR (zkbd.c).
static void type(const char *utf8) {
	const char *p = utf8, *end = utf8 + strlen(utf8);
	while (p < end) {
		uint32_t cp = z_utf8_next(&p, end);
		key(cp == '\n' ? 0x0d : cp);
	}
}

static void fresh(void) {
	buf_clear();
	nlines = 0;
	top_line = 0;
	sel_anchor = -1;
	rewrap_all();
}

// The column the caret is drawn in, in display columns, measured
// independently of text.c: z_cp_width() of every character before it.
static int caret_col_w(void) {
	int l = line_at(cursor);
	const char *p = &buf[line_off[l]], *end = &buf[cursor];
	int n = 0;
	while (p < end) n += z_cp_width(z_utf8_next(&p, end));
	return n;
}

// The column the caret is drawn in, measured the independent way: how
// many characters from its line's start.
static int caret_col(void) {
	int l = line_at(cursor);
	return (int)z_utf8_count(&buf[line_off[l]], (size_t)(cursor - (int)line_off[l]));
}

// ---------------------------------------------------------------

int main(int argc, char **argv) {

	memset(buf, 0, sizeof(buf));

	if (!z_render_open(&win, WIN_W, WIN_H)) {
		printf("text utf8: skipped (cannot map the VRAM address)\n");
		return 77;
	}
	if (!k_install()) {
		printf("text utf8: skipped (needs -no-pie and vm.mmap_min_addr=0)\n");
		return 77;
	}

	z_scrollbar_init(&sbar, &win, Z_SB_VERT);
	dlg_ctx.parent = &win;
	layout();

	// -- 1. typing --
	printf("1. typing non-ASCII\n");
	fresh();
	type("Gr\xC3\xBC\xC3\x9F" "e \xE2\x82\xAC" "5");		// "Grüße €5"
	expect(doc_is("Gr\xC3\xBC\xC3\x9F" "e \xE2\x82\xAC" "5"), "typed text is UTF-8");
	expect(cursor == len, "caret at the end");
	expect(caret_col() == 8, "caret column counts characters, not bytes");

	// -- 2. deleting whole characters --
	printf("2. Backspace, Delete, arrows over multi-byte characters\n");
	key(0x7f);											// deletes '5'
	key(0x7f);											// deletes the euro, 3 bytes
	expect(doc_is("Gr\xC3\xBC\xC3\x9F" "e "), "Backspace removes a whole euro");
	key(Z_KEY_LEFT);									// before ' '
	key(Z_KEY_LEFT);									// before 'e'
	key(Z_KEY_LEFT);									// before sharp s
	expect(cursor == 4, "Left steps over 2-byte characters");
	expect(caret_col() == 3, "caret column after Left");
	key(Z_KEY_DELETE);									// deletes sharp s
	expect(doc_is("Gr\xC3\xBC" "e "), "Delete removes a whole sharp s");
	key(Z_KEY_RIGHT);
	expect(cursor == 5, "Right steps one character");
	key(Z_KEY_HOME);
	key_m(Z_KEY_END, 0);
	expect(cursor == len, "End reaches the end");

	// -- 3. wrapping counts characters --
	printf("3. wrapping and Up/Down in character columns\n");
	fresh();
	// cols characters of a-umlaut exactly fill a line; one more wraps.
	for (int i = 0; i < cols + 3; i++) key(0xE4);
	expect(nlines == 2, "a line of 2-byte letters wraps at cols characters");
	expect(line_draw_len(0) == cols * 2, "first line holds cols letters (2 bytes each)");
	key(Z_KEY_UP);
	expect(line_at(cursor) == 0 && caret_col() == 3, "Up keeps the character column");

	// -- 4. a UTF-8 file round-trips, BOM included --
	printf("4. UTF-8 file, with byte order mark\n");
	{
		const char f[] = "\xEF\xBB\xBF" "caf\xC3\xA9\n";
		put_file("/U.TXT", f, (int)sizeof(f) - 1);
		fresh();
		expect(load_file("/U.TXT"), "loads");
		expect(file_enc == ENC_UTF8 && file_bom, "detected UTF-8 with BOM");
		expect(doc_is("caf\xC3\xA9\n"), "BOM not in the document");
		expect(do_save_to("/U.TXT"), "saves");
		expect(file_is("/U.TXT", f, (int)sizeof(f) - 1), "saved byte for byte, BOM back");
	}

	// -- 5. a Latin-9 file round-trips, every byte --
	printf("5. Latin-9 file: detected, edited, saved back as Latin-9\n");
	{
		unsigned char f[256];
		int n = 0;
		const char *head = "Gr\xFC\xDF" "e \xA4 ";		// Latin-9 "Grüße € "
		for (const char *p = head; *p; p++) f[n++] = (unsigned char)*p;
		// ...and every byte 0x80-0xFF, C1 included, which must survive.
		for (int b = 0x80; b <= 0xFF; b++) f[n++] = (unsigned char)b;
		f[n++] = '\n';
		put_file("/L.TXT", f, n);
		fresh();
		expect(load_file("/L.TXT"), "loads");
		expect(file_enc == ENC_LATIN9, "detected Latin-9");
		expect(!memcmp(buf, "Gr\xC3\xBC\xC3\x9F" "e \xE2\x82\xAC ", 12),
			"converted to UTF-8 in the buffer");
		expect(z_utf8_valid(buf, (size_t)len), "buffer is valid UTF-8");
		expect(do_save_to("/L.TXT"), "saves");
		expect(file_is("/L.TXT", f, n), "saved back byte for byte, all 256 values");

		// An edit that stays inside Latin-9.
		cursor = 0;
		type("\xC5\x92");									// OE ligature, 0xBC in Latin-9
		expect(latin9_unsavable() == 0, "OE ligature fits Latin-9");
		expect(do_save_to("/L.TXT"), "saves again");
		mfile_t *lf = find("/L.TXT", false);
		expect(lf && lf->size == n + 1 && lf->data[0] == 0xBC, "OE written as byte 0xBC");

		// One that does not: counted, so do_save_to() can ask.
		type("\xE3\x81\x82");								// hiragana a
		expect(latin9_unsavable() == 1, "hiragana cannot be saved as Latin-9");
		expect(write_document("/L2.TXT"), "Latin-9 write anyway");
		mfile_t *l2 = find("/L2.TXT", false);
		expect(l2 && l2->data[1] == '?', "written as '?'");
		file_enc = ENC_UTF8;								// what Yes does
		expect(write_document("/L3.TXT"), "UTF-8 write");
		mfile_t *l3 = find("/L3.TXT", false);
		expect(l3 && l3->size > 3 && !memcmp(&l3->data[2], "\xE3\x81\x82", 3),
			"hiragana kept in UTF-8");
	}

	// -- 6. a UTF-8 file with a stray bad byte stays UTF-8, byte exact --
	printf("6. UTF-8 file with a malformed byte\n");
	{
		const char f[] = "na\xC3\xAFve \xFF caf\xC3\xA9 \xC3\xA0 la\n";
		put_file("/B.TXT", f, (int)sizeof(f) - 1);
		fresh();
		expect(load_file("/B.TXT"), "loads");
		expect(file_enc == ENC_UTF8, "still UTF-8: 3 good sequences, 1 bad byte");
		// Caret movement over the bad byte: one character.
		cursor = 0;
		for (int i = 0; i < 6; i++) key(Z_KEY_RIGHT);	// n a i" v e space
		expect(cursor == 7, "Right over 'na\xC3\xAFve '");
		key(Z_KEY_RIGHT);
		expect(cursor == 8, "the bad byte is one character");
		expect(do_save_to("/B.TXT"), "saves");
		expect(file_is("/B.TXT", f, (int)sizeof(f) - 1), "bad byte saved unchanged");
	}

	// -- 7. a Latin-1 file that grows too large is refused --
	printf("7. too large once converted\n");
	{
		static unsigned char big[TEXT_MAX - 10];
		memset(big, 0xE4, sizeof(big));				// all a-umlaut
		put_file("/BIG.TXT", big, (int)sizeof(big));
		// load_file() raises a dialog here, which needs a running wm;
		// check the conversion step it relies on directly instead.
		fresh();
		memcpy(buf, big, sizeof(big));
		len = (int)sizeof(big);
		expect(!latin9_to_utf8_in_place(), "doubling past TEXT_MAX refused");
		expect(len == (int)sizeof(big) && (unsigned char)buf[0] == 0xE4,
			"buffer untouched when refused");
	}

	// -- 8. selection drawing with multi-byte text does not crash --
	printf("8. selection across multi-byte text renders\n");
	fresh();
	type("\xC3\xA9t\xC3\xA9 \xC3\xA0 Paris");
	sel_anchor = 2;
	cursor = 9;
	for (int r = 0; r < rows; r++) draw_row(r);
	expect(true, "drew rows with a selection");

	// -- 9. Japanese: two columns a character --
	printf("9. Japanese: wide characters, wrapping, caret columns\n");
	fresh();
	type("\xE3\x81\x82\xE3\x81\x84\xE3\x81\x86");			// あいう
	expect(caret_col_w() == 6, "three kana are six columns");
	key(Z_KEY_LEFT);
	expect(caret_col_w() == 4, "Left steps back one kana, two columns");
	key(0x7f);
	expect(doc_is("\xE3\x81\x82\xE3\x81\x86"), "Backspace removes a whole kana");
	fresh();
	// cols/2 kana fill a line exactly; one more wraps to the next.
	for (int i = 0; i < cols / 2 + 1; i++) type("\xE6\x97\xA5");	// 日
	expect(nlines == 2, "a line of kanji wraps at cols columns");
	expect(line_draw_len(0) == (cols / 2) * 3, "first line holds cols/2 kanji");
	fresh();
	// An odd number of columns left: a wide character does not straddle
	// the edge, it moves down whole.
	for (int i = 0; i < cols - 1; i++) key('x');
	type("\xE6\x97\xA5");
	expect(nlines == 2 && line_draw_len(1) == 3, "wide character at the edge moves down whole");
	fresh();
	type("a\xE6\x97\xA5" "b");
	key(Z_KEY_UP);											// one line: stays
	key(Z_KEY_HOME);
	key(Z_KEY_RIGHT);
	key(Z_KEY_RIGHT);
	expect(caret_col_w() == 3, "caret after a, kanji: column 3");

	// -- 11. undo and redo (docs/text_editor.md, "Undo") --
	printf("11. undo and redo\n");
	fresh();
	type("hello world");
	expect(modified, "typing modifies");
	do_undo(false);
	expect(doc_is("") && cursor == 0, "one undo takes back a run of typing");
	expect(!modified, "back to the empty document: unmodified");
	do_undo(true);
	expect(doc_is("hello world") && cursor == len, "redo puts it back");

	fresh();
	type("abc\ndef");
	do_undo(false);
	expect(doc_is("abc\n"), "Enter ends a group: the second line goes first");
	do_undo(false);
	expect(doc_is(""), "then the first");
	do_undo(true);
	do_undo(true);
	expect(doc_is("abc\ndef"), "redo both");

	fresh();
	type("hello");
	key(0x7f); key(0x7f); key(0x7f);
	expect(doc_is("he"), "three backspaces");
	do_undo(false);
	expect(doc_is("hello") && cursor == 5, "a run of Backspace undoes as one, caret back");
	do_undo(false);
	expect(doc_is(""), "then the typing");

	fresh();
	type("hello world");
	sel_anchor = 6;
	cursor = 11;
	type("X");
	expect(doc_is("hello X"), "typing over a selection");
	do_undo(false);
	expect(doc_is("hello world"), "one undo puts the selection back");

	fresh();
	type("Gr\xC3\xBC\xC3\x9F" "e \xE6\x97\xA5");
	key(0x7f);
	do_undo(false);
	expect(doc_is("Gr\xC3\xBC\xC3\x9F" "e \xE6\x97\xA5"), "undo a deleted kanji, whole");
	do_undo(false);
	expect(doc_is(""), "and the multibyte typing");

	// Undo after a save stops at the saved text, which is unmodified.
	fresh();
	type("saved");
	expect(write_document("/U2.TXT"), "save");
	set_modified(false);
	z_undo_saved(&undo);
	undo_break();
	type(" more");
	do_undo(false);
	expect(doc_is("saved") && !modified, "undo back to the save: unmodified");

	// -- 12. find (docs/text_editor.md, "Find") --
	printf("12. find\n");
	fresh();
	type("Hello world, hello Welt, Gr\xC3\xBC\xC3\x9F" "e");
	cursor = 0;
	snprintf(find_text, sizeof(find_text), "HELLO");
	expect(find_from(0) && sel_start() == 0 && sel_end() == 5, "finds, either case");
	find_next();
	expect(sel_start() == 13 && sel_end() == 18, "find again moves on");
	find_next();
	expect(sel_start() == 0, "and wraps round");
	snprintf(find_text, sizeof(find_text), "gr\xC3\xBC\xC3\x9F" "e");
	expect(find_from(0) && sel_start() == 25, "finds a German word");
	snprintf(find_text, sizeof(find_text), "nowhere");
	expect(!find_from(0), "reports no match");

	// -- 10. a picture, for looking at: `utf8 /tmp/text` writes
	// /tmp/text-utf8.pbm -- Latin-9 in hardware glyphs, and the box for
	// what Latin-9 has not got (the half, the hiragana).
	if (argc > 1) {
		// The 6x12 font, where kanji can be drawn, and the real
		// Japanese font from the tree.
		static uint8_t jf[200 * 1024];
		FILE *ff = fopen("sw/data/font/jp12.zfn", "rb");
		size_t jn = ff ? fread(jf, 1, sizeof(jf), ff) : 0;
		if (ff) fclose(ff);
		z_jfont_use(jf, (uint32_t)jn);
		cur_font = &z_font_6x12;
		layout();
		fresh();
		type("Gr\xC3\xBC\xC3\x9F" "e aus K\xC3\xB6ln \xE2\x80\x94 15 \xE2\x82\xAC\n");
		type("\xC3\xA9t\xC3\xA9, \xC3\xB1" "and\xC3\xBA, \xC5\x93uvre, \xC3\xA7" "a\n");
		type("not in Latin-9: \xC2\xBD \xE2\x80\x94\n");
		type("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE3\x81\xA7\xE6\x9B\xB8\xE3\x81\x8F: "
			"\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF\xE3\x80\x81"
			"\xE3\x82\xBC\xE3\x82\xA4\xE3\x83\x88\xE3\x83\xAD\xE3\x82\xB9\n");
		z_render_clear();
		for (int r = 0; r < rows; r++) draw_row(r);
		draw_caret();
		char path[256];
		snprintf(path, sizeof(path), "%s-utf8.pbm", argv[1]);
		z_render_write(path, &win, 2);
		printf("wrote %s\n", path);
	}

	printf("text utf8: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;

}
