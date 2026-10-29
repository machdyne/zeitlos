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
#define TEXT_HOST_TEST
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

#define NFILES  24
#define FILEMAX (2 << 20)

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
static bool no_ramdisk;
static uint32_t ticks = 1000;

// Z_SYS_ARENA, on two regions of the low 4GB that it alternates
// between on every change, contents copied: so every resize is a MOVE,
// the case text.c has to get right (zeitlos.h, z_arena_set()). Off
// until a test turns it on -- with it off the editor is on a board with
// no arena, which is what the earlier sections are about.
#define ARENA_MAX (16u << 20)
static bool arena_on;
static uint8_t *arena_r[2];
static int arena_which;
static uint32_t arena_size, arena_moves, arena_limit = ARENA_MAX;

static int32_t k_arena_stub(z_arena_args_t *a) {
	if (!arena_on) return Z_ARENA_E_PERM;
	if (a->size != Z_ARENA_QUERY) {
		uint32_t want = (a->size + 4095u) & ~4095u;
		if (want > arena_limit) return Z_ARENA_E_RESERVE;
		if (!arena_r[0]) {
			for (int i = 0; i < 2; i++) {
				arena_r[i] = mmap(NULL, ARENA_MAX, PROT_READ | PROT_WRITE,
					MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
				if (arena_r[i] == MAP_FAILED) { arena_r[i] = NULL; return Z_ARENA_E_NOMEM; }
			}
		}
		if (want != arena_size) {
			uint8_t *from = arena_r[arena_which], *to = arena_r[!arena_which];
			memcpy(to, from, want < arena_size ? want : arena_size);
			memset(from, 0xA5, ARENA_MAX);		// the old place is gone
			arena_which = !arena_which;
			arena_size = want;
			arena_moves++;
		}
	}
	a->base = arena_size ? (uint32_t)(uintptr_t)arena_r[arena_which] : 0;
	a->size = arena_size;
	return Z_ARENA_OK;
}

// Processes, for autosave's untitled copies (section 21): this one is
// my_pid with serial my_serial; live[] says which other pids run, with
// what serial.
static uint32_t my_pid = 7, my_serial = 100;
static uint32_t live_serial[16];

// Messages for the mailbox, and after how many reads they arrive.
static struct { uint32_t subject, v; } mq[8];
static int mq_n;
static int reads_until_mail = -1;	// FS reads before mq is let in; -1: at once
static long chunk_reads;			// fs_read_chunk() calls, all told

static int card_left = -1;		// bytes the card can still take; -1: no limit

// The blitter's memory-copy mode (zgfx.h): what big print needs. On by
// default, as on every current bitstream; section 23 turns it off.
static bool blit_mem_ok = true;
bool z_fb_hw_blit_mem_available(void) { return blit_mem_ok; }
static int rename_fail_at;		// the Nth rename from now fails (0: none)

static uint32_t *k_syscall(uint32_t id, uint32_t *args, uint32_t b) {

	(void)b;

	switch (id) {

	case Z_SYS_UPTIME:
		((z_obj_t *)args)->type = Z_UINT32;
		((z_obj_t *)args)->val.uint32 = ticks++;
		return (uint32_t *)&k_ok;

	// Empty, unless a test queued something (section 22: a key pressed
	// while a file opens).
	case Z_SYS_MSG_READ: {
		if (!mq_n || reads_until_mail > 0) return (uint32_t *)&k_fail;
		z_msg_t *m = (z_msg_t *)args;
		memset(m, 0, sizeof(*m));
		m->subject = mq[0].subject;
		m->obj.type = Z_UINT32;
		m->obj.val.uint32 = mq[0].v;
		memmove(mq, mq + 1, sizeof(mq[0]) * (size_t)--mq_n);
		return (uint32_t *)&k_ok;
	}

	case Z_SYS_GETPID:
		((z_obj_t *)args)->type = Z_UINT32;
		((z_obj_t *)args)->val.uint32 = my_pid;
		return (uint32_t *)&k_ok;

	case Z_SYS_PROC_SERIAL: {
		z_proc_serial_args_t *a = (z_proc_serial_args_t *)args;
		a->serial = a->pid == my_pid ? my_serial : (a->pid < 16 ? live_serial[a->pid] : 0);
		return (uint32_t *)&k_ok;
	}

	// A directory's files: every name under `path` + "/".
	case Z_SYS_FS_LIST: {
		z_fs_list_args_t *a = (z_fs_list_args_t *)args;
		size_t pl = strlen(a->path), o = 0;
		a->count = 0;
		a->truncated = 0;
		for (int i = 0; i < NFILES; i++) {
			if (!files[i].used || strncmp(files[i].name, a->path, pl) || files[i].name[pl] != '/') continue;
			size_t k = strlen(files[i].name) + 1;
			if (o + k > a->out_cap || a->count >= a->max_entries) { a->truncated = 1; break; }
			memcpy(a->out + o, files[i].name, k);
			o += k;
			a->count++;
		}
		return (uint32_t *)&k_ok;
	}

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
		if (reads_until_mail > 0) reads_until_mail--;
		chunk_reads++;
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
		// A card that fills up (section 20): writes past card_left fail.
		if (card_left >= 0 && (int)a->len > card_left) return (uint32_t *)&k_fail;
		if (card_left >= 0) card_left -= (int)a->len;
		if (open_file->size + (int)a->len > FILEMAX) return (uint32_t *)&k_fail;
		memcpy(&open_file->data[open_file->size], a->buf, a->len);
		open_file->size += (int)a->len;
		a->written = a->len;
		return (uint32_t *)&k_ok;
	}

	case Z_SYS_FS_CLOSE:
		open_file = NULL;
		return (uint32_t *)&k_ok;

	// /ram exists unless a test says not (autosave, section 19).
	case Z_SYS_FS_STAT:
		return (uint32_t *)(no_ramdisk ? &k_fail : &k_ok);

	// Whole files, unlink and rename: what autosave uses (section 19).
	case Z_SYS_FS_READ: {
		z_fs_read_args_t *a = (z_fs_read_args_t *)args;
		mfile_t *f = find(a->name, false);
		if (!f || f->size > (int)a->maxlen) return (uint32_t *)&k_fail;
		memcpy(a->buf, f->data, (size_t)f->size);
		a->len = (uint32_t)f->size;
		return (uint32_t *)&k_ok;
	}

	case Z_SYS_FS_WRITE: {
		z_fs_write_args_t *a = (z_fs_write_args_t *)args;
		mfile_t *f = find(a->name, true);
		if (!f || (int)a->len > FILEMAX) return (uint32_t *)&k_fail;
		memcpy(f->data, a->buf, a->len);
		f->size = (int)a->len;
		a->written = a->len;
		return (uint32_t *)&k_ok;
	}

	case Z_SYS_FS_UNLINK: {
		z_fs_unlink_args_t *a = (z_fs_unlink_args_t *)args;
		mfile_t *f = find(a->name, false);
		if (!f) return (uint32_t *)&k_fail;
		f->used = false;
		return (uint32_t *)&k_ok;
	}

	case Z_SYS_FS_RENAME: {
		z_fs_rename_args_t *a = (z_fs_rename_args_t *)args;
		mfile_t *f = find(a->from, false);
		if (rename_fail_at && --rename_fail_at == 0) { a->err = 0; return (uint32_t *)&k_fail; }
		if (!f || find(a->to, false)) { a->err = Z_FS_ERR_EXIST; return (uint32_t *)&k_fail; }
		snprintf(f->name, sizeof(f->name), "%s", a->to);
		a->err = 0;
		return (uint32_t *)&k_ok;
	}

	case Z_SYS_ARENA: {
		z_arena_args_t *a = (z_arena_args_t *)args;
		a->result = k_arena_stub(a);
		return (uint32_t *)(a->result == Z_ARENA_OK ? &k_ok : &k_fail);
	}

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
	doc_flat();
	return len == (int)strlen(s) && !memcmp(doc, s, (size_t)len);
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
	const char *p = P(LO(l)), *end = P(LO(l)) + (cursor - LO(l));
	int n = 0;
	while (p < end) n += z_cp_width(z_utf8_next(&p, end));
	return n;
}

// The column the caret is drawn in, measured the independent way: how
// many characters from its line's start.
static int caret_col(void) {
	int l = line_at(cursor);
	return (int)z_utf8_count(P(LO(l)), (size_t)(cursor - LO(l)));
}

// ---------------------------------------------------------------

int main(int argc, char **argv) {

	memset(sbuf, 0, sizeof(sbuf));

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
		// ...and every byte 0xA0-0xFF, and the C1 bytes that
		// Windows-1252 leaves undefined, which must survive. (A C1 byte
		// Windows-1252 does define -- a curly quote, a dash -- makes the
		// file Windows-1252 instead: section 13.)
		for (int b = 0x80; b <= 0xFF; b++)
			if (b >= 0xA0 || !cp1252_hi[b - 0x80]) f[n++] = (unsigned char)b;
		f[n++] = '\n';
		put_file("/L.TXT", f, n);
		fresh();
		expect(load_file("/L.TXT"), "loads");
		expect(file_enc == ENC_LATIN9, "detected Latin-9");
		expect(!memcmp(doc, "Gr\xC3\xBC\xC3\x9F" "e \xE2\x82\xAC ", 12),
			"converted to UTF-8 in the buffer");
		expect(z_utf8_valid(doc, (size_t)len), "buffer is valid UTF-8");
		expect(do_save_to("/L.TXT"), "saves");
		expect(file_is("/L.TXT", f, n), "saved back byte for byte, every value");

		// An edit that stays inside Latin-9.
		cursor = 0;
		type("\xC5\x92");									// OE ligature, 0xBC in Latin-9
		expect(sb_unsavable() == 0, "OE ligature fits Latin-9");
		expect(do_save_to("/L.TXT"), "saves again");
		mfile_t *lf = find("/L.TXT", false);
		expect(lf && lf->size == n + 1 && lf->data[0] == 0xBC, "OE written as byte 0xBC");

		// One that does not: counted, so do_save_to() can ask.
		type("\xE3\x81\x82");								// hiragana a
		expect(sb_unsavable() == 1, "hiragana cannot be saved as Latin-9");
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
		memcpy(doc, big, sizeof(big));
		len = (int)sizeof(big);
		gs = len;
		gl = cap - len;
		expect(!sb_to_utf8_in_place(), "doubling past TEXT_MAX refused");
		expect(len == (int)sizeof(big) && (unsigned char)doc[0] == 0xE4,
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

	// -- 13. Windows-1252 (docs/text_editor.md, "Encodings") --
	printf("13. Windows-1252 file: detected, round-tripped byte for byte\n");
	{
		// Notepad's "ANSI": curly quotes, an en dash, the euro at 0x80,
		// a-umlaut at 0xE4 -- and every other byte, undefined ones too.
		unsigned char f[300];
		int n = 0;
		const unsigned char head[] = { 0x93, 'H', 'i', 0x94, ' ', 0x96, ' ', 0x80,
			'5', ' ', 0xE4, ' ', 0xA4, ' ' };
		for (unsigned i = 0; i < sizeof head; i++) f[n++] = head[i];
		for (int b = 0x80; b <= 0xFF; b++) f[n++] = (unsigned char)b;
		f[n++] = '\n';
		put_file("/W.TXT", f, n);
		fresh();
		expect(load_file("/W.TXT"), "loads");
		expect(file_enc == ENC_CP1252, "detected Windows-1252");
		expect(!memcmp(doc, "\xE2\x80\x9CHi\xE2\x80\x9D \xE2\x80\x93 \xE2\x82\xAC" "5 "
			"\xC3\xA4 \xC2\xA4 ", 23), "quotes, dash, euro, a-umlaut and currency sign in UTF-8");
		expect(z_utf8_valid(doc, (size_t)len), "buffer is valid UTF-8");
		expect(do_save_to("/W.TXT"), "saves");
		expect(file_is("/W.TXT", f, n), "saved back byte for byte, all 256 values");
		cursor = 0;
		type("\xE2\x80\x94");								// em dash: 0x97
		expect(sb_unsavable() == 0, "em dash fits Windows-1252");
		expect(do_save_to("/W.TXT"), "saves again");
		mfile_t *wf = find("/W.TXT", false);
		expect(wf && wf->size == n + 1 && wf->data[0] == 0x97, "em dash written as 0x97");
		type("\xC5\x92");									// OE ligature: 0x8C here
		expect(sb_unsavable() == 0 && sb_cp_to_byte(0x152) == 0x8C, "OE ligature is 0x8C");
		type("\xE3\x81\x82");
		expect(sb_unsavable() == 1, "hiragana cannot be saved as Windows-1252");

		// Latin-9 is still Latin-9: no 0x80-0x9f bytes, so no tell.
		const unsigned char l9[] = { 'G', 'r', 0xFC, 0xDF, 'e', ' ', 0xA4, '\n' };
		put_file("/L9.TXT", l9, (int)sizeof l9);
		fresh();
		expect(load_file("/L9.TXT") && file_enc == ENC_LATIN9, "no C1 bytes: Latin-9");
		expect(!memcmp(&doc[len - 4], "\xE2\x82\xAC\n", 4), "0xA4 is the euro in Latin-9");
	}

	// -- 14. replace (docs/text_editor.md, "Replace") --
	printf("14. replace\n");
	fresh();
	type("one two One three one");
	snprintf(find_text, sizeof(find_text), "one");
	snprintf(repl_text, sizeof(repl_text), "1");
	expect(count_matches(true) == 3, "three matches, either case");
	cursor = 22;
	expect(replace_all_undoable(3), "three replacements can be undone");
	expect(replace_all_core(true) == 3, "replace all: three");
	expect(doc_is("1 two 1 three 1"), "shorter replacement, one pass");
	expect(cursor == len, "caret at the end stays at the end");
	do_undo(false);
	expect(doc_is("one two One three one"), "one undo puts all three back, case and all");
	do_undo(true);
	expect(doc_is("1 two 1 three 1"), "redo replaces them again");

	fresh();
	type("a-b-c");
	snprintf(find_text, sizeof(find_text), "-");
	snprintf(repl_text, sizeof(repl_text), " \xE2\x80\x94 ");	// " — "
	cursor = 2;													// after "a-"
	expect(replace_all_core(true) == 2, "longer replacement");
	expect(doc_is("a \xE2\x80\x94 b \xE2\x80\x94 c"), "grows in place, from the top of the buffer");
	expect(cursor == 6, "caret stays on the b, after the first replacement");
	do_undo(false);
	expect(doc_is("a-b-c"), "undone");

	fresh();
	type("x, y, z");
	snprintf(find_text, sizeof(find_text), ", ");
	repl_text[0] = 0;
	expect(replace_all_core(true) == 2 && doc_is("xyz"), "replace with nothing deletes");
	do_undo(false);
	expect(doc_is("x, y, z"), "and comes back");

	fresh();
	type("aaaa");
	snprintf(find_text, sizeof(find_text), "aa");
	snprintf(repl_text, sizeof(repl_text), "b");
	expect(count_matches(true) == 2 && replace_all_core(true) == 2 && doc_is("bb"),
		"matches do not overlap");

	// Too many to undo: the history is cleared rather than left half.
	fresh();
	for (int i = 0; i < 300; i++) type("ab ");
	snprintf(find_text, sizeof(find_text), "ab");
	snprintf(repl_text, sizeof(repl_text), "c");
	expect(!replace_all_undoable(300), "300 replacements are more than the log holds");
	expect(replace_all_core(false) == 300 && len == 600, "replaced anyway");
	expect(!z_undo_can_undo(&undo) && modified, "history cleared, still modified");

	// Would not fit: nothing changes.
	fresh();
	memset(doc, 'q', TEXT_MAX - 10);
	len = TEXT_MAX - 10;
	gs = len;
	gl = cap - len;
	rewrap_all();
	snprintf(find_text, sizeof(find_text), "q");
	snprintf(repl_text, sizeof(repl_text), "qq");
	expect(replace_all_core(false) == -1 && len == TEXT_MAX - 10 && doc[0] == 'q',
		"a replace that would overflow changes nothing");

	// One at a time: replace the selected match.
	fresh();
	type("cat hat cat");
	snprintf(find_text, sizeof(find_text), "cat");
	snprintf(repl_text, sizeof(repl_text), "dog");
	expect(find_from(4) && sel_start() == 8, "finds the second cat");
	expect(replace_selection() && doc_is("cat hat dog") && cursor == 11, "replaces just that one");
	do_undo(false);
	expect(doc_is("cat hat cat"), "undone on its own");

	// -- 15. go to line, counts (docs/text_editor.md, "Go to line") --
	printf("15. go to line and counts\n");
	fresh();
	type("first line\nsecond line here\n\nfourth \xC3\xBC\n");
	{
		int w, c;
		doc_counts(&w, &c);
		expect(w == 7, "seven words");
		doc_flat();
		expect(c == (int)z_utf8_count(doc, (size_t)len), "characters, not bytes");
		expect(doc_line_of(len) == 5, "five lines, the last one empty");
		goto_line(2);
		expect(cursor == 11 && doc_line_of(cursor) == 2, "line 2 starts after the first newline");
		goto_line(4);
		expect(cursor == 29, "line 4, after the blank line 3");
		goto_line(99);
		expect(doc_line_of(cursor) == 5 && cursor == len, "past the end: the last line");
		goto_line(0);
		expect(cursor == 0, "line 0 is line 1");
	}

	// -- 16. the gap and the line table, against a model --
	// (docs/text_editor.md, "The document"). Random edits -- typing,
	// newlines, deletes across paragraphs, pastes -- at random places;
	// after each, the text must be the model's and the line table the
	// one a full rewrap builds.
	printf("16. random edits: text and line table against a model\n");
	{
		static char model[200000];
		int mlen = 0, bad_text = 0, bad_lines = 0;
		srand(42);
		fresh();
		for (int step = 0; step < 4000; step++) {
			int at = len ? rand() % (len + 1) : 0;
			int kind = rand() % 10;
			if (kind < 7) {
				char piece[64];
				int n = 1 + rand() % (kind < 4 ? 3 : 60);
				for (int i = 0; i < n; i++) {
					int r = rand() % 20;
					piece[i] = r == 0 ? '\n' : r < 4 ? ' ' : (char)('a' + r);
				}
				if (len + n > TEXT_MAX - 100) continue;
				undo_begin(UK_OTHER, at);
				int para = para_line_at(at);
				edit_insert(at, piece, n);
				cursor = at + n;
				after_edit(para);
				memmove(&model[at + n], &model[at], (size_t)(mlen - at));
				memcpy(&model[at], piece, (size_t)n);
				mlen += n;
			} else {
				int b = at + 1 + rand() % (kind == 9 ? 120 : 8);
				if (b > len) b = len;
				if (b <= at) continue;
				undo_begin(UK_OTHER, at);
				int para = para_line_at(at);
				edit_delete(at, b);
				cursor = at;
				after_edit(para);
				memmove(&model[at], &model[b], (size_t)(mlen - b));
				mlen -= b - at;
			}
			if (!layout_check()) bad_lines++;
			if ((step & 63) == 0) {
				int gs0 = gs;
				bool same = len == mlen;
				for (int i = 0; same && i < len; i++) same = B(i) == model[i];
				if (!same) bad_text++;
				expect(gs0 == gs, "reading moves no gap");
			}
		}
		expect(bad_text == 0, "text matches the model after every edit");
		expect(bad_lines == 0, "line table matches a full rewrap after every edit");
		expect(gs == len || B(gs - 1) == '\n', "the gap is at the end of a paragraph");
		printf("   %d bytes, %d lines\n", len, nlines);
	}

	// -- 16c. editing while the document is still being wrapped in
	// the background: edits before the frontier, a move past it, Replace
	// All restarting it -- the table matches a full rewrap at the end.
	printf("16c. edits during background wrapping\n");
	{
		static char big_text[30000];
		int n = 0;
		while (n < (int)sizeof(big_text) - 100)
			n += snprintf(&big_text[n], sizeof(big_text) - (size_t)n,
				"Sentence number %d goes on for a while before it ends.%s", n, (n % 7) ? " " : "\n");
		put_file("/BG.TXT", big_text, n);
		fresh();
		expect(load_file("/BG.TXT"), "loads");
		expect(wrap_bg, "the rest of it is still to wrap");
		int f0 = len - wrap_rest;
		cursor = 10; type("early ");				// before the frontier
		expect(wrap_bg && len - wrap_rest == f0 + 6, "the frontier moved with the text before it");
		key(Z_KEY_PAGEDOWN); key(Z_KEY_PAGEDOWN);	// page into what was not wrapped
		type("paged ");
		wrap_more(-1, 3000);
		cursor = len / 2;							// beyond the frontier
		type("middle ");
		expect(layout_check(), "the table is the full rewrap's");
		snprintf(find_text, sizeof(find_text), "goes on");
		snprintf(repl_text, sizeof(repl_text), "runs on");
		replace_all_core(true);
		expect(wrap_bg || nlines > 0, "Replace All wraps the rest in the background too");
		cursor = len; type(" the end");
		expect(layout_check(), "and after Replace All and an edit at the end");
		do_undo(false); do_undo(false);
		expect(layout_check(), "and after undoing both");
	}

	// -- 16b. undo and redo through the gap: random edits taken back
	// one group at a time, and put back, with the line table checked at
	// every step against a full rewrap.
	printf("16b. undo and redo of random edits, through the gap\n");
	{
		static char start[8000], final_text[9000];
		int bad = 0;
		srand(7);
		fresh();
		for (int i = 0; i < 120; i++) type(i % 9 == 8 ? "line\n" : "word ");
		doc_flat();
		int slen = len;
		memcpy(start, doc, (size_t)len);
		z_undo_reset(&undo);
		undo_break();
		for (int step = 0; step < 60; step++) {
			int at = rand() % (len + 1);
			undo_begin(UK_OTHER, at);
			int para = para_line_at(at);
			if (rand() % 3) {
				char piece[24];
				int n = 1 + rand() % 20;
				for (int k = 0; k < n; k++) piece[k] = (rand() % 7) ? (char)('a' + rand() % 26) : (rand() % 2 ? ' ' : '\n');
				edit_insert(at, piece, n);
				cursor = at + n;
			} else {
				int b = at + 1 + rand() % 30;
				if (b > len) b = len;
				edit_delete(at, b);
				cursor = at;
			}
			undo_break();
			after_edit(para);
		}
		doc_flat();
		int flen = len;
		memcpy(final_text, doc, (size_t)len);
		while (z_undo_can_undo(&undo)) { do_undo(false); if (!layout_check()) bad++; }
		doc_flat();
		expect(len == slen && !memcmp(doc, start, (size_t)len), "undo all: the starting text");
		while (z_undo_can_redo(&undo)) { do_undo(true); if (!layout_check()) bad++; }
		doc_flat();
		expect(len == flen && !memcmp(doc, final_text, (size_t)len), "redo all: the final text");
		expect(bad == 0, "line table right after every undo and redo");
	}

	// -- 17. a document past TEXT_MAX, in the arena --
	printf("17. a large document: the arena\n");
	{
		arena_on = true;
		static unsigned char f[400000];
		int n = 0;
		for (int para = 0; n < (int)sizeof(f) - 200; para++) {
			n += snprintf((char *)&f[n], sizeof(f) - (size_t)n,
				"Paragraph %d of a long document, with enough words in it to wrap"
				" across a few display lines at any sensible width. \xC3\xA4\xC3\xB6\xC3\xBC.\n", para);
		}
		put_file("/LONG.TXT", f, n);
		fresh();
		expect(load_file("/LONG.TXT"), "a 400KB file loads");
		expect(big && len == n, "in the arena, every byte");
		expect(layout_check(), "line table built");
		int lines0 = nlines;
		expect(lines0 > MAX_LINES, "more display lines than the .bss table holds");

		// typing at the start, the middle and the end
		uint32_t moves0 = arena_moves;
		cursor = 0;
		type("Start. ");
		cursor = len / 2;
		type("Middle\n");
		cursor = len;
		type("End.");
		expect(layout_check(), "line table right after typing in three places");
		doc_flat();
		expect(!memcmp(doc, "Start. Paragraph 0", 18), "typed at the start");
		expect(!memcmp(&doc[len - 4], "End.", 4), "typed at the end");
		printf("   %d arena moves while typing\n", (int)(arena_moves - moves0));

		// undo all three
		do_undo(false); do_undo(false); do_undo(false);
		doc_flat();				// undo goes through the gap, which may sit anywhere
		expect(len == n && !memcmp(doc, f, (size_t)n), "three undos: the file again");
		expect(layout_check(), "line table after undo");

		// typing in a few hundred places
		for (int i = 0; i < 200; i++) {
			cursor = (int)(((long)len * i) / 200);
			type("inserted words ");
		}
		expect(layout_check(), "line table after typing all over");

		// growing past the arena: big pastes, which move it
		{
			static char chunk[60000];
			for (int i = 0; i < (int)sizeof(chunk); i++)
				chunk[i] = (i % 61 == 60) ? '\n' : (char)('a' + i % 26);
			for (int i = 0; i < 12; i++) {
				cursor = (int)(((long)len * (i + 1)) / 13);
				int para = para_line_at(cursor);
				expect(doc_room((int)sizeof(chunk)), "room for a 60KB paste");
				undo_begin(UK_OTHER, cursor);
				edit_insert(cursor, chunk, (int)sizeof(chunk));
				cursor += (int)sizeof(chunk);
				undo_break();
				after_edit(para);
			}
			expect(len > n + 700000, "over a megabyte");
		}
		expect(layout_check(), "line table after growing through moves");
		expect(arena_moves > moves0, "the arena moved and the editor followed");

		// replace all across the whole thing
		snprintf(find_text, sizeof(find_text), "Paragraph");
		snprintf(repl_text, sizeof(repl_text), "Absatz");
		int m = count_matches(true);
		expect(m > 3000 && replace_all_core(false) == m, "replace all in the arena");
		expect(layout_check(), "line table after replace all");

		// save and read back
		expect(do_save_to("/LONG2.TXT"), "saves");
		mfile_t *lf = find("/LONG2.TXT", false);
		doc_flat();
		expect(lf && lf->size == len && !memcmp(lf->data, doc, (size_t)len),
			"saved byte for byte, through the staging buffer");

		// a new document goes back to .bss and gives the arena back
		fresh();
		expect(!big && doc == sbuf && arena_size == 0, "new: back in .bss, arena freed");

		// no arena to be had: as it always was
		arena_limit = 64 * 1024;
		expect(!load_file("/LONG.TXT") && len == 0, "too large where the arena cannot grow");
		arena_limit = ARENA_MAX;
		arena_on = false;
	}

	// -- 18. what a keystroke costs, in a megabyte --
	// (docs/text_editor.md, "Typing in a large document"). The work per
	// key is the edited paragraph -- moved, wrapped -- plus one
	// addition per line below; never the document. And random edits on
	// the large document still match a model.
	printf("18. a keystroke in a megabyte, and random edits on it\n");
	{
		arena_on = true;
		static unsigned char f[1100000];
		int n = 0;
		for (int para = 0; n < (int)sizeof(f) - 200; para++)
			n += snprintf((char *)&f[n], sizeof(f) - (size_t)n,
				"Paragraph %d, long enough to wrap onto a second display line"
				" in the default window and then some more words.\n", para);
		put_file("/MEG.TXT", f, n);
		fresh();
		expect(load_file("/MEG.TXT") && big, "a 1.1MB file loads into the arena");

		// Loading wraps only the first screen and a slice; the main
		// loop wraps the rest between messages. Counted here: how much
		// was done before the window could be drawn, and how many
		// slices the rest took.
		{
			int first = nlines, slices = 0;
			expect(wrap_bg && first < 1000, "loading wraps the first screen, not the megabyte");
			while (wrap_bg) { wrap_more(-1, WRAP_SLICE); slices++; }
			printf("   loading: %d lines before the first draw, the rest in %d slices of %d bytes\n",
				first, slices, WRAP_SLICE);
			expect(layout_check(), "the table the slices built is the full rewrap's");
		}

		struct { const char *where; int at; } spots[] = {
			{ "start", 0 }, { "middle", n / 2 }, { "end", n - 1 },
		};
		for (unsigned k = 0; k < 3; k++) {
			cursor = spots[k].at;
			key('x');				// the gap comes here: once
			cost_moved = cost_wrapped = cost_shifted = 0;
			for (int i = 0; i < 100; i++) key('a' + i % 26);
			printf("   typing at the %s: %ld bytes moved, %ld wrapped, %ld lines shifted per key (%d lines)\n",
				spots[k].where, cost_moved / 100, cost_wrapped / 100, cost_shifted / 100, nlines);
			expect(cost_moved / 100 < 400, "a key moves its paragraph, not the document");
			expect(cost_wrapped / 100 < 400, "a key wraps its paragraph, not the document");
			expect(cost_shifted / 100 <= nlines, "and shifts at most the lines below");
		}
		expect(layout_check(), "line table right");

		static char model[1300000];
		doc_flat();
		memcpy(model, doc, (size_t)len);
		int mlen = len, bad = 0;
		srand(7);
		for (int step = 0; step < 600; step++) {
			int at = rand() % (len + 1);
			if (rand() % 3) {
				char piece[40];
				int k = 1 + rand() % 40;
				for (int i = 0; i < k; i++) piece[i] = (rand() % 12) ? (char)('a' + rand() % 26) : '\n';
				if (!doc_room(k)) break;
				int para = para_line_at(at);
				undo_begin(UK_OTHER, at);
				edit_insert(at, piece, k);
				cursor = at + k;
				after_edit(para);
				memmove(&model[at + k], &model[at], (size_t)(mlen - at));
				memcpy(&model[at], piece, (size_t)k);
				mlen += k;
			} else {
				int b = at + 1 + rand() % 200;
				if (b > len) b = len;
				if (b <= at) continue;
				int para = para_line_at(at);
				undo_begin(UK_OTHER, at);
				edit_delete(at, b);
				cursor = at;
				after_edit(para);
				memmove(&model[at], &model[b], (size_t)(mlen - b));
				mlen -= b - at;
			}
			if (step % 100 == 99 && !layout_check()) bad++;
		}
		bool same = len == mlen;
		for (int i = 0; same && i < len; i++) same = B(i) == model[i];
		expect(same, "random edits on the large document match the model");
		expect(bad == 0 && layout_check(), "and its line table a full rewrap");
		fresh();
		arena_on = false;
	}

	// -- 19. autosave (docs/text_editor.md, "Autosave") --
	printf("19. autosave: after a pause, in pieces, abandoned by an edit\n");
	{
		static char body[20000];
		for (int i = 0; i < (int)sizeof(body); i++) body[i] = (i % 50 == 49) ? '\n' : (char)('a' + i % 26);
		put_file("/home/A.TXT", body, (int)sizeof(body));
		fresh();
		expect(load_file("/home/A.TXT"), "loads");
		char sav[40], inf[40], nw[40];
		as_name(sav, sizeof(sav), filename, "SAV");
		as_name(inf, sizeof(inf), filename, "INF");
		as_name(nw, sizeof(nw), filename, "NEW");
		expect(!strncmp(sav, "/ram/T", 6) && strlen(sav) == 17, "an 8.3 name on the RAM disk");
		expect(!autosave_step(), "unmodified: nothing to do");

		cursor = 0;
		type("edited ");
		expect(!autosave_step(), "not straight after an edit");
		ticks += AUTOSAVE_IDLE + 1;
		expect(autosave_step(), "after the pause: started");
		expect(autosave_step(), "a piece written");
		type("more ");								// an edit mid-way
		expect(!autosave_step() && !find(nw, false), "an edit abandons the copy");
		ticks += AUTOSAVE_IDLE + 1;
		int steps = 0;
		while (autosave_step() && steps < 100) steps++;
		expect(steps >= 5, "written in pieces of 4KB");
		doc_flat();
		mfile_t *sf = find(sav, false), *nf = find(inf, false);
		expect(sf && sf->size == len && !memcmp(sf->data, doc, (size_t)len), "the copy is the document");
		expect(!find(nw, false), "and complete: the .NEW renamed");
		expect(nf && !strncmp((char *)nf->data, "/home/A.TXT\n0 0\n", 16), "the note: which file, which encoding");
		expect(!autosave_step(), "nothing more until the next edit");

		// Saving removes it
		expect(do_save_to("/home/A.TXT"), "save");
		expect(!find(sav, false) && !find(inf, false), "saved: the copy is gone");

		// A copy in the arena, across the gap
		arena_on = true;
		static char bigb[100000];
		for (int i = 0; i < (int)sizeof(bigb); i++) bigb[i] = (i % 61 == 60) ? '\n' : (char)('A' + i % 26);
		put_file("/home/B.TXT", bigb, 60000);
		fresh();
		expect(load_file("/home/B.TXT") && big, "a document in the arena");
		cursor = 30000;
		type("in the middle");
		ticks += AUTOSAVE_IDLE + 1;
		while (autosave_step()) ;
		as_name(sav, sizeof(sav), filename, "SAV");
		sf = find(sav, false);
		doc_flat();
		expect(sf && sf->size == len && !memcmp(sf->data, doc, (size_t)len),
			"copied from the arena, across the gap, byte for byte");
		autosave_discard();
		expect(!find(sav, false), "discarded");
		arena_on = false;

		// No RAM disk: no copy at all -- never to the card.
		fresh();
		expect(load_file("/home/A.TXT"), "loads again");
		as_ram = -1;
		no_ramdisk = true;
		type("x");
		ticks += AUTOSAVE_IDLE + 1;
		int nfiles_before = 0, nfiles_after = 0;
		for (int i = 0; i < NFILES; i++) nfiles_before += files[i].used;
		expect(!autosave_step(), "no RAM disk: nothing written");
		for (int i = 0; i < NFILES; i++) nfiles_after += files[i].used;
		expect(nfiles_after == nfiles_before, "not a file anywhere");
		no_ramdisk = false;
		as_ram = -1;
		fresh();
	}

	printf("20. saving: written to NAME.part, then renamed\n");
	{
		// A file on the card, then an edited document saved over it.
		fresh();
		put_file("/home/S.TXT", "the old file\n", 13);
		expect(load_file("/home/S.TXT"), "loads");
		type("new words ");
		expect(do_save_to("/home/S.TXT"), "saves");
		mfile_t *f = find("/home/S.TXT", false);
		expect(f && f->size == 23 && !memcmp(f->data, "new words the old file\n", 23),
			"the file is the document");
		expect(!find("/home/S.TXT.part", false), "and no .part is left");

		// The card fills up mid-save: the old file is as it was.
		type("more ");
		char part[Z_FS_PATH_MAX];
		card_left = 10;
		expect(write_safely("/home/S.TXT", part, (int)sizeof(part)) == SAVE_FAILED,
			"a full card fails the save");
		card_left = -1;
		f = find("/home/S.TXT", false);
		expect(f && f->size == 23 && !memcmp(f->data, "new words the old file\n", 23),
			"and the file on the card is untouched");
		expect(!find("/home/S.TXT.part", false), "and the half-written .part is gone");

		// A rename that fails at once (not for EXIST): the old file is
		// still there, so the .part goes and nothing has changed.
		rename_fail_at = 1;
		expect(write_safely("/home/S.TXT", part, (int)sizeof(part)) == SAVE_FAILED,
			"a rename refused at once fails the save");
		f = find("/home/S.TXT", false);
		expect(f && f->size == 23, "and leaves the old file");
		expect(!find("/home/S.TXT.part", false), "and no .part");

		// The old file deleted, then the rename fails: the .part is the
		// only copy, complete, and kept.
		rename_fail_at = 2;
		expect(write_safely("/home/S.TXT", part, (int)sizeof(part)) == SAVE_KEPT_PART,
			"a rename that fails after the delete keeps the .part");
		rename_fail_at = 0;
		mfile_t *pf = find("/home/S.TXT.part", false);
		expect(pf && pf->size == len, "and the .part is the whole document");
		expect(!find("/home/S.TXT", false), "under its own name (the old file went)");
		if (pf) pf->used = false;

		// A file not there yet: written, renamed, no delete needed.
		expect(do_save_to("/home/NEW.TXT"), "a new file saves");
		f = find("/home/NEW.TXT", false);
		expect(f && f->size == len, "whole");
		expect(!find("/home/NEW.TXT.part", false), "no .part");
		fresh();
	}

	printf("21. autosave of untitled documents: a copy each, offered only once its window is gone\n");
	{
		fresh();
		for (int i = 0; i < NFILES; i++)
			if (files[i].used && !strncmp(files[i].name, "/ram/", 5)) files[i].used = false;

		// Window A (pid 7): an untitled document, autosaved.
		my_pid = 7; my_serial = 100; as_untitled[0] = 0;
		filename[0] = 0;
		type("window A's words");
		ticks += AUTOSAVE_IDLE + 1;
		while (autosave_step()) ;
		char a_sav[40];
		as_name(a_sav, sizeof(a_sav), as_key(), "SAV");
		expect(find(a_sav, false) != NULL, "A's copy is in /ram");

		// Window B (pid 8) starts while A runs: A's copy is not offered
		// (a dialog would be: this harness has no wm, so a dialog here
		// would never return -- reaching the end of the call is the check)
		live_serial[7] = 100;
		my_pid = 8; my_serial = 200; as_untitled[0] = 0;
		fresh();
		autosave_offer();
		expect(find(a_sav, false) != NULL, "a second window leaves A's copy alone");
		type("window B's words");
		ticks += AUTOSAVE_IDLE + 1;
		while (autosave_step()) ;
		char b_sav[40];
		as_name(b_sav, sizeof(b_sav), as_key(), "SAV");
		expect(strcmp(a_sav, b_sav) != 0, "B's copy has a slot of its own");
		mfile_t *af = find(a_sav, false);
		expect(af && af->size == 16 && !memcmp(af->data, "window A's words", 16),
			"and A's copy is still A's");

		// A is gone (crashed): B saving does not delete it, and its key
		// is now an orphan's.
		live_serial[7] = 0;
		expect(as_orphan("untitled 7 100"), "A's copy is an orphan once A is gone");
		expect(!as_orphan("untitled 8 200"), "B's own is not");
		live_serial[7] = 300;
		expect(as_orphan("untitled 7 100"), "nor is pid 7 used again by another process");
		expect(as_orphan(""), "an older text's copy is an orphan");
		live_serial[7] = 0;
		my_pid = 7; my_serial = 100;
		fresh();
	}

	printf("22. opening in pieces: CRLF across them, Esc stops\n");
	{
		// Pieces are 1000 bytes here: a CRLF split across a piece, a
		// lone CR at the end of one, a CR at the end of the file.
		static char big2[6000], want2[6000];
		int bn = 5200, wn = 0;
		for (int i = 0; i < bn; i++) big2[i] = (char)('a' + i % 26);
		for (int i = 96; i < bn; i += 97) { big2[i] = '\r'; big2[i + 1] = '\n'; }
		big2[999] = '\r'; big2[1000] = '\n';		// split across the first two pieces
		big2[1999] = '\r'; big2[2000] = 'q';		// a lone CR ending a piece
		big2[bn++] = '\r';							// a CR that ends the file
		// the reference: CRLF folded, every other CR kept
		for (int i = 0; i < bn; i++) {
			if (big2[i] == '\r' && i + 1 < bn && big2[i + 1] == '\n') continue;
			want2[wn++] = big2[i];
		}
		expect(big2[999] == '\r' && big2[1000] == '\n', "a CRLF straddles the first piece's end");
		put_file("/home/P.TXT", big2, bn);
		expect(load_file("/home/P.TXT"), "loads");
		bool same = len == wn;
		for (int i = 0; same && i < wn; i++) if (B(i) != want2[i]) same = false;
		expect(same, "CRLF folded across pieces; lone and final CR kept");

		// Esc after the first few pieces: nothing opened, nothing named.
		static char big3[400000];
		for (int i = 0; i < (int)sizeof(big3); i++) big3[i] = (char)(i % 61 == 60 ? '\n' : 'a' + i % 26);
		put_file("/home/E.TXT", big3, (int)sizeof(big3));
		arena_on = true;				// 400 KB: the arena's
		fresh();
		filename[0] = 0;
		mq[0].subject = Z_WM_KEY; mq[0].v = Z_WM_PACK_KEY(0x1b, 0, 1); mq_n = 1;
		reads_until_mail = 5;
		long r0 = chunk_reads;
		expect(!load_file("/home/E.TXT"), "Esc stops the open");
		expect(chunk_reads - r0 > 5 && chunk_reads - r0 < 60,
			"after a few of its 400 pieces, not at the end");
		expect(len == 0 && !filename[0], "and leaves an empty, untitled document");
		expect(!io_busy, "with the window drawing again");
		reads_until_mail = -1;
		mq_n = 0;

		// a click meanwhile is let go, the open completes
		mq[0].subject = Z_WM_MOUSE; mq[0].v = Z_WM_PACK_MOUSE(10, 10, 1, 1); mq_n = 1;
		reads_until_mail = 5;
		bool ok2 = load_file("/home/E.TXT");
		if (!ok2 || len != (int)sizeof(big3)) printf("    load %d len %d stopped %d\n", ok2, len, io_stopped);
		expect(ok2 && len == (int)sizeof(big3), "a click does not stop it");
		reads_until_mail = -1;
		mq_n = 0;

		// Saving it: the mailbox is read meanwhile (wm's redraws are
		// served), and a key pressed then is not typed into what is
		// being written.
		mq[0].subject = Z_WM_KEY; mq[0].v = Z_WM_PACK_KEY('z', 0, 1); mq_n = 1;
		expect(do_save_to("/home/E2.TXT"), "a 400 KB save");
		expect(mq_n == 0, "the mailbox was read during it");
		mfile_t *ef = find("/home/E2.TXT", false);
		expect(ef && ef->size == (int)sizeof(big3) && !memcmp(ef->data, big3, sizeof(big3)),
			"and the file is the document, without the key");
		mq_n = 0;

		// Find through it: the same answer as before, in slices, and
		// Esc stops a search that has not found anything yet.
		snprintf(find_text, sizeof(find_text), "%s", "nomatchanywhere");
		uint32_t t0 = ticks;
		expect(find_from(0) == 0, "a find through 400 KB that finds nothing");
		expect(ticks - t0 > 20, "read its mail on the way (the clock was asked)");
		mq[0].subject = Z_WM_KEY; mq[0].v = Z_WM_PACK_KEY(0x1b, 0, 1); mq_n = 1;
		expect(find_from(0) == -1, "Esc stops it");
		expect(!has_sel(), "selecting nothing");
		mq_n = 0;
		snprintf(find_text, sizeof(find_text), "%s", "nopqr");
		expect(find_from(200000) == 1 && sel_start() >= 200000, "a real match past the middle");
		mq[0].subject = Z_WM_KEY; mq[0].v = Z_WM_PACK_KEY(0x1b, 0, 1); mq_n = 1;
		snprintf(find_text, sizeof(find_text), "%s", "nomatchanywhere");
		expect(count_matches(true) == -1, "Esc stops a count too");
		mq_n = 0;
		arena_on = false;
		fresh();
	}

	printf("23. big print: the third size, a line at a time from memory\n");
	{
		// The reference: m10x20.zfn read here, independently of
		// tools/gen_bigfont.py and zbigline.c -- its rows MSB-first, as
		// the file has them.
		static uint8_t zf[16384];
		FILE *ff = fopen("sw/data/font/zfn/m10x20.zfn", "rb");
		if (!ff) ff = fopen("../../data/font/zfn/m10x20.zfn", "rb");
		size_t zn = ff ? fread(zf, 1, sizeof(zf), ff) : 0;
		if (ff) fclose(ff);
		expect(zn > 12 && !memcmp(zf, "ZFN1", 4), "the reference font is readable");
		int rw = zf[4], rh = zf[5], rs = zf[6];
		uint32_t rcount = (uint32_t)zf[8] | (uint32_t)zf[9] << 8 | (uint32_t)zf[10] << 16 | (uint32_t)zf[11] << 24;
		const uint8_t *rcps = zf + 12;
		uint32_t roff = 12 + 2 * rcount;
		roff += (4 - roff % 4) % 4;

		// the cycle: 5x8, 6x12, big, 5x8
		fresh();
		cur_font = &z_font_5x8; big_font = NULL;
		do_font();
		expect(cur_font == &z_font_6x12 && !big_font, "Aa: 5x8 -> 6x12");
		do_font();
		expect(big_font == &z_bigfont_10x20, "Aa again: 6x12 -> big print");
		expect(CHAR_W == 10 && LINE_H == 21, "big print's cell is 10x20, a line 21");
		expect(cols == (text_w - TEXT_X0) / 10, "and the columns follow from it");

		// a line on a screen full of ink: every pixel of the row is
		// rewritten, so none of the old ink survives between letters
		type("Gr\xC3\xBC\xC3\x9F" "e 15 \xE2\x82\xAC, ok");	// "Grüße 15 €, ok"
		z_clip_t c;
		z_win_content_rect(&win, &c);
		for (int y = c.y0; y < c.y0 + LINE_H; y++)
			for (int x = c.x0; x < c.x0 + text_w; x++)
				z_fb_set_pixel(x, y, 1, NULL);
		draw_row(0);
		const char *line = "Gr\xC3\xBC\xC3\x9F" "e 15 \xE2\x82\xAC, ok";
		int bad = 0, ch_i = 0;
		const char *lp = line, *le = line + strlen(line);
		while (lp < le) {
			uint32_t cp = z_utf8_next(&lp, le);
			int gi = -1;
			for (uint32_t k = 0; k < rcount; k++)
				if ((uint32_t)(rcps[2 * k] | rcps[2 * k + 1] << 8) == cp) { gi = (int)k; break; }
			for (int y = 0; y < rh; y++)
				for (int x = 0; x < rw; x++) {
					int want = 0;
					if (gi >= 0) {
						const uint8_t *row = zf + roff + ((uint32_t)gi * rh + y) * rs;
						int v = row[0] << 8 | (rs > 1 ? row[1] : 0);
						want = (v >> (15 - x)) & 1;
					}
					if (z_render_get(c.x0 + TEXT_X0 + ch_i * rw + x, c.y0 + y) != want) bad++;
				}
			ch_i++;
		}
		expect(bad == 0, "the line is the font's own pixels, letter for letter");
		int stray = 0;
		for (int x = c.x0 + TEXT_X0 + ch_i * rw; x < c.x0 + text_w; x++)
			for (int y = c.y0; y < c.y0 + LINE_H; y++) stray += z_render_get(x, y);
		for (int x = c.x0; x < c.x0 + TEXT_X0; x++)
			for (int y = c.y0; y < c.y0 + LINE_H; y++) stray += z_render_get(x, y);
		for (int x = c.x0; x < c.x0 + text_w; x++) stray += z_render_get(x, c.y0 + rh);
		expect(stray == 0, "and the rest of the row is background, the old ink gone");

		// a selection: those cells, and only those, inverted
		sel_anchor = 0; cursor = 2;			// "Gr"
		draw_row(0);
		int inv_ok = 1;
		for (int x = 0; x < 2 * rw && inv_ok; x++)
			for (int y = 0; y < rh; y++) {
				int on = z_render_get(c.x0 + TEXT_X0 + x, c.y0 + y);
				const uint8_t *row;
				int gi = (x < rw) ? 'G' : 'r', k;
				for (k = 0; k < (int)rcount; k++)
					if ((rcps[2 * k] | rcps[2 * k + 1] << 8) == gi) break;
				row = zf + roff + ((uint32_t)k * rh + y) * rs;
				int want = !(((row[0] << 8 | row[1]) >> (15 - x % rw)) & 1);
				if (on != want) { inv_ok = 0; break; }
			}
		expect(inv_ok, "a selection is the same letters, inverted");
		expect(z_render_get(c.x0 + TEXT_X0 + 2 * rw + 1, c.y0 + 1) == 0, "and stops where it ends");
		sel_anchor = -1;

		// a wide character the font has not got: a box two cells wide;
		// a long line stops at the scrollbar
		fresh();
		type("\xE6\x97\xA5x");				// 日x
		draw_row(0);
		int boxed = 0;
		for (int x = 0; x < 2 * rw; x++) boxed += z_render_get(c.x0 + TEXT_X0 + x, c.y0 + rh / 2);
		expect(boxed == 2, "a character it has not got is a box, two cells wide");
		fresh();
		for (int i = 0; i < 200; i++) type("w");
		for (int y = c.y0; y < c.y0 + LINE_H; y++)
			for (int x = c.x0 + text_w; x <= c.x1; x++) z_fb_set_pixel(x, y, 0, NULL);
		draw_row(0);
		int over = 0;
		for (int y = c.y0; y < c.y0 + LINE_H; y++)
			for (int x = c.x0 + text_w; x <= c.x1; x++) over += z_render_get(x, y);
		expect(over == 0, "a full line stops short of the scrollbar");

		// once more: back to 5x8
		do_font();
		expect(!big_font && cur_font == &z_font_5x8, "Aa a third time: back to 5x8");

		// a bitstream without the memory copy: two sizes, as before
		blit_mem_ok = false;
		do_font(); do_font();
		expect(!big_font && cur_font == &z_font_5x8, "without the memory copy, Aa keeps to two sizes");
		blit_mem_ok = true;
		fresh();
	}

	printf("24. rows drawn without clearing them first: nothing left behind\n");
	{
		// Each row drawn over a screen full of ink must come out the
		// same as on a clean screen: draw_row() no longer blanks the
		// row before drawing it (that blank was the flash on every
		// keystroke), so whatever its characters do not paint, it must.
		const z_font_t *fonts[2] = { &z_font_5x8, &z_font_6x12 };
		z_clip_t c;
		static uint8_t clean[Z_SCREEN_H][Z_SCREEN_W];
		for (int fi = 0; fi < 2; fi++) {
			big_font = NULL;
			cur_font = fonts[fi];
			layout();
			fresh();
			type("short\nGr\xC3\xBC\xC3\x9F" "e \xE6\x97\xA5\xE6\x9C\xAC x\n"
				"a longer line that runs on\n\nlast");
			z_win_content_rect(&win, &c);
			for (int pass = 0; pass < 2; pass++) {
				sel_anchor = pass ? 3 : -1;		// the second time, a selection
				cursor = pass ? 12 : cursor;
				for (int r = 0; r < 6; r++) {
					int y0 = c.y0 + r * LINE_H;
					for (int y = y0; y < y0 + LINE_H; y++)
						for (int x = c.x0; x < c.x0 + text_w; x++)
							z_fb_set_pixel(x, y, 0, NULL);
					draw_row(r);
					for (int y = y0; y < y0 + LINE_H; y++)
						for (int x = c.x0; x < c.x0 + text_w; x++)
							clean[y][x] = (uint8_t)z_render_get(x, y);
					for (int y = y0; y < y0 + LINE_H; y++)
						for (int x = c.x0; x < c.x0 + text_w; x++)
							z_fb_set_pixel(x, y, 1, NULL);
					draw_row(r);
					int diff = 0;
					for (int y = y0; y < y0 + LINE_H; y++)
						for (int x = c.x0; x < c.x0 + text_w; x++)
							diff += clean[y][x] != (uint8_t)z_render_get(x, y);
					char what[96];
					snprintf(what, sizeof(what), "%dx%d, row %d%s: drawn over ink, the same as on a clean screen",
						cur_font->w, cur_font->h, r, pass ? ", with a selection" : "");
					expect(diff == 0, what);
				}
			}
			sel_anchor = -1;
		}
		cur_font = &z_font_5x8;
		layout();
		fresh();
	}

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
		type("not in Latin-9: \xC2\xBD \xE3\x83\xBB\n");
		type("look-alikes: \xE2\x80\x9CWindows\xE2\x80\x9D \xE2\x80\x98quotes\xE2\x80\x99 \xE2\x80\x93 dashes \xE2\x80\x94 \xE2\x80\xA2 bullets\xE2\x80\xA6\n");
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

		// big print, as the third Aa size draws it
		z_jfont_use(NULL, 0);
		cur_font = &z_font_5x8;
		big_font = &z_bigfont_10x20;
		layout();
		fresh();
		type("Dear Grandma,\n\ntoday we saw a fox in the forest. It was red and very "
			"fast. Dad says it lives under the old oak by the stream.\n\n"
			"Can we go again on Saturday?\n\nLove, Mia");
		z_render_clear();
		repaint();
		snprintf(path, sizeof(path), "%s-big.pbm", argv[1]);
		z_render_write(path, &win, 2);
		printf("wrote %s\n", path);
		big_font = NULL;
	}

	printf("text utf8: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;

}
