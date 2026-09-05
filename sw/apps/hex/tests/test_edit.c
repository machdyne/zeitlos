/*
 * Editing tests for sw/apps/hex -- the journal, save, and undo.
 *
 *   cc -std=gnu99 -Wall -no-pie -I sw/common -o /tmp/test_edit \
 *      sw/apps/hex/tests/test_edit.c \
 *      sw/common/zwin.c sw/common/zwidget.c sw/common/zfont_data.c \
 *      sw/common/zobj.c sw/common/zeitlos.c
 *   sudo sysctl -w vm.mmap_min_addr=0     # once per boot
 *   /tmp/test_edit
 *
 * The -no-pie and the sysctl are both needed by install_trampoline()
 * below, which explains why. Without either, the test exits 77 and
 * says so rather than crashing.
 *
 * tests/render.c shows what the grid LOOKS like and tests/test_layout.c
 * checks where things are. Neither can see any of this: whether an edit
 * survives the cache being reloaded under it, whether Save writes the
 * right bytes to the right offsets, whether two edits to one byte
 * resolve to the later one, whether undo puts back what was there.
 *
 * All of those fail silently. An edit lost to a cache reload looks like
 * the user mistyping; a save that writes a superseded value produces a
 * file that is wrong in a way nothing on screen ever showed.
 *
 * The stub filesystem below COUNTS seeks and writes, so the run
 * coalescing can be asserted rather than assumed -- "it wrote the right
 * bytes" is true of the naive one-write-per-byte version too.
 */

#include "../../../common/tests/zrender.h"
#include "trampoline.h"

#define main hex_main_unused
#include "../hex.c"
#undef main

// -- stub filesystem ---------------------------------------------

#define STUB_SIZE  0x8000u

static uint8_t disk[STUB_SIZE];
static uint32_t stub_pos;
static int n_seeks, n_writes, n_reads;
static bool write_fails_after;      // fail every write past this many
static int writes_before_failure;

static void disk_init(void) {
	for (uint32_t i = 0; i < STUB_SIZE; i++) disk[i] = (uint8_t)(i & 0xff);
	stub_pos = 0;
	n_seeks = n_writes = n_reads = 0;
	write_fails_after = false;
	writes_before_failure = 0;
}

int fs_size(char *n) { (void)n; return (int)STUB_SIZE; }
int fs_open_rw(const char *n) { (void)n; stub_pos = 0; return 3; }
int fs_open_read(const char *n) { (void)n; stub_pos = 0; return 3; }
int fs_close_handle(int h) { (void)h; return 1; }
int fs_sync(int h) { (void)h; return 1; }
int fs_truncate(int h, uint32_t s) { (void)h; (void)s; return 1; }
int fs_touch(const char *p) { (void)p; return 1; }

bool fs_df(uint32_t *t, uint32_t *f) {
	if (t) *t = 1u << 20;
	if (f) *f = 1u << 20;
	return true;
}

int fs_seek(int h, uint32_t off) {
	(void)h;
	n_seeks++;
	stub_pos = off > STUB_SIZE ? STUB_SIZE : off;
	return 1;
}

int fs_read_chunk(int h, void *buf, int maxlen) {
	(void)h;
	n_reads++;
	uint32_t left = STUB_SIZE - stub_pos;
	uint32_t n = (uint32_t)maxlen < left ? (uint32_t)maxlen : left;
	memcpy(buf, &disk[stub_pos], n);
	stub_pos += n;
	return (int)n;
}

int fs_write_chunk(int h, const void *buf, int len) {
	(void)h;
	if (write_fails_after && n_writes >= writes_before_failure) return -1;
	n_writes++;
	uint32_t n = (uint32_t)len;
	if (stub_pos + n > STUB_SIZE) n = STUB_SIZE - stub_pos;
	memcpy(&disk[stub_pos], buf, n);
	stub_pos += n;
	return (int)n;
}

// -- dialogs -----------------------------------------------------
//
// z_dialog_confirm() returning CANCEL matters: do_save()'s failure path
// shows one, and a stub that blocked or returned YES would change what
// is being tested.

bool z_dialog_open(const z_dialog_ctx_t *c, const char *d, char *o, int n) {
	(void)c; (void)d; (void)o; (void)n; return false; }
bool z_dialog_save(const z_dialog_ctx_t *c, const char *d, const char *s,
	char *o, int n) { (void)c; (void)d; (void)s; (void)o; (void)n; return false; }
int z_dialog_confirm(const z_dialog_ctx_t *c, const char *t, const char *m,
	int b) { (void)c; (void)t; (void)m; (void)b; return Z_DIALOG_CANCEL; }
bool z_dialog_prompt(const z_dialog_ctx_t *c, const char *t, const char *m,
	const char *i, char *o, int n) {
	(void)c; (void)t; (void)m; (void)i; (void)o; (void)n; return false; }

// -- harness -----------------------------------------------------

static int failures, checks;

#define CHECK(cond, ...) do { \
	checks++; \
	if (!(cond)) { \
		printf("FAIL %s:%d: ", __FILE__, __LINE__); \
		printf(__VA_ARGS__); \
		printf("\n"); \
		failures++; \
	} \
} while (0)

static void session(void) {

	disk_init();

	memset(&win, 0, sizeof(win));
	win.id = 1;
	win.w = WIN_W;
	win.h = WIN_H;

	z_scrollbar_init(&sbar, &win, Z_SB_VERT);
	cur_font = &z_font_5x8;

	fh = 3;
	fsize = STUB_SIZE;
	read_only = false;
	cursor = 0;
	top_row = 0;
	in_ascii = false;
	nibble_low = false;
	status_msg = NULL;

	jrn_clear();
	cache_drop();
	layout();

}

// -- tests -------------------------------------------------------

static void t_edit_is_visible(void) {

	session();

	CHECK(byte_at(0x100) == 0x00, "precondition: byte 0x100 is 0x%02x",
		byte_at(0x100));

	put_byte(0x100, 0xAB, false);

	CHECK(byte_at(0x100) == 0xAB, "edit not visible: 0x%02x", byte_at(0x100));
	CHECK(modified(), "edit did not mark the document modified");
	CHECK(jrn_n == 1, "journal holds %d entries, expected 1", jrn_n);
	CHECK(disk[0x100] == 0x00, "edit reached the card before Save");

}

// The one that would fail silently. The card still holds the old byte,
// so a reload reverts the display while the journal still claims the
// edit exists.
static void t_edit_survives_cache_reload(void) {

	session();

	put_byte(0x40, 0x11, false);
	put_byte(0x41, 0x22, false);

	// Force the cache somewhere else entirely, then come back.
	CHECK(byte_at(0x7000) == (0x7000 & 0xff), "far read wrong");
	CHECK(!cache_valid || cache_off > 0x40, "cache did not actually move");

	CHECK(byte_at(0x40) == 0x11, "edit lost to a cache reload: 0x%02x",
		byte_at(0x40));
	CHECK(byte_at(0x41) == 0x22, "edit lost to a cache reload: 0x%02x",
		byte_at(0x41));

}

static void t_save_writes_and_coalesces(void) {

	session();

	// Sixteen consecutive bytes, entered out of order on purpose --
	// nothing guarantees a user edits left to right, and the sort is
	// what turns this into one run.
	for (int i = 15; i >= 0; i--)
		put_byte(0x200 + (uint32_t)i, (uint8_t)(0xF0 | i), false);

	int seeks_before = n_seeks, writes_before = n_writes;

	CHECK(do_save(), "save reported failure");

	for (int i = 0; i < 16; i++)
		CHECK(disk[0x200 + i] == (0xF0 | i),
			"byte 0x%x is 0x%02x, expected 0x%02x",
			0x200 + i, disk[0x200 + i], 0xF0 | i);

	CHECK(n_writes - writes_before == 1,
		"16 consecutive bytes took %d writes, expected 1 coalesced run",
		n_writes - writes_before);
	CHECK(n_seeks - seeks_before == 1,
		"16 consecutive bytes took %d seeks, expected 1",
		n_seeks - seeks_before);

	CHECK(!modified(), "still modified after a successful save");
	CHECK(jrn_n == 0, "journal not cleared after save");

}

static void t_save_scattered(void) {

	session();

	const uint32_t offs[] = { 0x7FF0, 0x10, 0x4000, 0x11, 0x0 };

	for (unsigned i = 0; i < sizeof(offs) / sizeof(offs[0]); i++)
		put_byte(offs[i], 0x5A, false);

	int writes_before = n_writes;

	CHECK(do_save(), "save reported failure");

	for (unsigned i = 0; i < sizeof(offs) / sizeof(offs[0]); i++)
		CHECK(disk[offs[i]] == 0x5A, "byte 0x%x is 0x%02x, expected 0x5A",
			offs[i], disk[offs[i]]);

	// 0x10 and 0x11 are adjacent and coalesce; the other three do not.
	CHECK(n_writes - writes_before == 4,
		"scattered save took %d writes, expected 4", n_writes - writes_before);

	// Nothing else moved. A save that writes the right bytes AND some
	// wrong ones passes every other assertion here.
	int stray = 0;
	for (uint32_t i = 0; i < STUB_SIZE; i++) {
		bool expected = false;
		for (unsigned k = 0; k < sizeof(offs) / sizeof(offs[0]); k++)
			if (offs[k] == i) expected = true;
		if (!expected && disk[i] != (uint8_t)(i & 0xff)) stray++;
	}
	CHECK(stray == 0, "%d bytes changed that should not have", stray);

}

// Two edits to one byte must resolve to the LATER one. Writing both
// would be harmless only if they happened to be written in order, and
// the sort does not promise that for equal keys unless it is told to.
static void t_last_edit_wins(void) {

	session();

	put_byte(0x80, 0x01, false);
	put_byte(0x80, 0x02, false);
	put_byte(0x80, 0x03, false);

	CHECK(byte_at(0x80) == 0x03, "display shows 0x%02x, expected 0x03",
		byte_at(0x80));

	int writes_before = n_writes;

	CHECK(do_save(), "save reported failure");
	CHECK(disk[0x80] == 0x03, "card holds 0x%02x, expected 0x03", disk[0x80]);
	CHECK(n_writes - writes_before == 1,
		"three edits to one byte took %d writes, expected 1",
		n_writes - writes_before);

}

static void t_undo(void) {

	session();

	uint8_t orig = (uint8_t)byte_at(0x300);

	put_byte(0x300, 0xEE, false);
	put_byte(0x301, 0xDD, false);

	do_undo();

	CHECK(byte_at(0x301) == (0x301 & 0xff),
		"undo left 0x301 as 0x%02x", byte_at(0x301));
	CHECK(byte_at(0x300) == 0xEE, "undo went too far: 0x300 is 0x%02x",
		byte_at(0x300));
	CHECK(jrn_n == 1, "journal holds %d after one undo, expected 1", jrn_n);

	do_undo();

	CHECK(byte_at(0x300) == orig, "undo left 0x300 as 0x%02x, expected 0x%02x",
		byte_at(0x300), orig);
	CHECK(!modified(), "still modified after undoing everything");

	// Undoing past the beginning must be a no-op, not an underflow.
	do_undo();
	CHECK(jrn_n == 0, "undo past the start left jrn_n = %d", jrn_n);

}

// Two edits to the same byte, then undo, must restore the FIRST edit's
// value -- not the original. The stack is LIFO and each entry's `old`
// is what was there when it was made.
static void t_undo_stacked(void) {

	session();

	put_byte(0x90, 0xA1, false);
	put_byte(0x90, 0xA2, false);

	do_undo();

	CHECK(byte_at(0x90) == 0xA1, "undo restored 0x%02x, expected 0xA1",
		byte_at(0x90));

}

// After a save that wrote some runs and then failed, undo cannot simply
// pop: the popped byte may already be on the card, and popping would
// leave the display and the file disagreeing with nothing left to
// reconcile them. It appends an inverse edit instead.
static void t_undo_after_partial_save(void) {

	session();

	put_byte(0x400, 0x77, false);
	put_byte(0x600, 0x88, false);

	// Let the first run through, fail the second.
	write_fails_after = true;
	writes_before_failure = n_writes + 1;

	CHECK(!do_save(), "save should have reported failure");
	CHECK(partial_save, "partial_save not set after a failed save");
	CHECK(modified(), "journal discarded after a failed save");

	write_fails_after = false;

	int before = jrn_n;
	do_undo();

	CHECK(jrn_n == before + 1,
		"undo after a partial save popped (%d -> %d) instead of appending "
		"an inverse edit", before, jrn_n);

	// And the correction actually reaches the card on the next save.
	CHECK(do_save(), "retry save failed");
	CHECK(disk[0x600] == (uint8_t)(0x600 & 0xff),
		"undone byte is 0x%02x on the card, expected the original 0x%02x",
		disk[0x600], (uint8_t)(0x600 & 0xff));

}

// A failed save keeps the journal precisely so it can be retried, and
// every write is idempotent so retrying is safe.
static void t_save_retry(void) {

	session();

	for (int i = 0; i < 4; i++)
		put_byte(0x500 + (uint32_t)i * 0x100, 0x33, false);

	write_fails_after = true;
	writes_before_failure = n_writes + 2;

	CHECK(!do_save(), "save should have failed");
	CHECK(jrn_n == 4, "journal lost entries on failure: %d", jrn_n);

	write_fails_after = false;

	CHECK(do_save(), "retry failed");
	CHECK(!modified(), "still modified after a successful retry");

	for (int i = 0; i < 4; i++)
		CHECK(disk[0x500 + i * 0x100] == 0x33,
			"byte 0x%x is 0x%02x after retry", 0x500 + i * 0x100,
			disk[0x500 + i * 0x100]);

}

static void t_read_only_refuses(void) {

	session();
	read_only = true;

	put_byte(0x10, 0xFF, false);

	CHECK(!modified(), "an edit was accepted on a read-only file");
	CHECK(byte_at(0x10) == 0x10, "read-only file changed on screen");

}

static void t_journal_full(void) {

	session();

	for (int i = 0; i < JRN_MAX + 8; i++)
		put_byte((uint32_t)i, 0x42, false);

	CHECK(jrn_n == JRN_MAX, "journal holds %d, expected the cap %d",
		jrn_n, JRN_MAX);

	// The refusal must not have corrupted anything: the first JRN_MAX
	// bytes took, the rest did not.
	CHECK(byte_at(JRN_MAX - 1) == 0x42, "last accepted edit missing");
	CHECK(byte_at(JRN_MAX) == (uint8_t)(JRN_MAX & 0xff),
		"a refused edit was applied anyway");

	CHECK(do_save(), "save of a full journal failed");

	for (int i = 0; i < JRN_MAX; i++)
		CHECK(disk[i] == 0x42, "byte 0x%x is 0x%02x after save", i, disk[i]);

}

// Typing "4" then "C" over a byte gives 0x4C, and the caret only
// advances on the second digit.
static void t_nibble_entry(void) {

	session();
	cursor = 0x50;

	handle_key('4', 0);

	CHECK(nibble_low, "first digit did not arm the low nibble");
	CHECK(cursor == 0x50, "caret advanced after the first digit");
	CHECK((byte_at(0x50) & 0xf0) == 0x40,
		"high nibble is 0x%02x", byte_at(0x50));

	handle_key('c', 0);

	CHECK(!nibble_low, "low nibble still armed after the second digit");
	CHECK(cursor == 0x51, "caret did not advance after the second digit");
	CHECK(byte_at(0x50) == 0x4C, "byte is 0x%02x, expected 0x4C",
		byte_at(0x50));

	// A half-typed byte must be abandoned by any caret movement, not
	// carried to the next offset.
	handle_key('7', 0);
	CHECK(nibble_low, "precondition");
	handle_key(Z_KEY_RIGHT, 0);
	CHECK(!nibble_low, "caret movement did not abandon the half-typed byte");

}

static void t_ascii_entry(void) {

	session();
	cursor = 0x60;
	in_ascii = true;

	handle_key('Z', 0);

	CHECK(byte_at(0x60) == 'Z', "character pane wrote 0x%02x", byte_at(0x60));
	CHECK(cursor == 0x61, "caret did not advance");

	// Non-printable input has no character to type; the hex pane is
	// what that is for.
	uint32_t was = cursor;
	handle_key(Z_KEY_F1, 0);
	CHECK(cursor == was, "a non-character key moved the caret");

}

int main(void) {

	if (!z_render_open(&win, WIN_W, WIN_H)) {
		printf("test_edit: skipped (cannot map the VRAM address)\n");
		return 77;
	}

	if (!z_tramp_install()) {
		printf("test_edit: skipped\n");
		return 77;
	}

	t_edit_is_visible();
	t_edit_survives_cache_reload();
	t_save_writes_and_coalesces();
	t_save_scattered();
	t_last_edit_wins();
	t_undo();
	t_undo_stacked();
	t_undo_after_partial_save();
	t_save_retry();
	t_read_only_refuses();
	t_journal_full();
	t_nibble_entry();
	t_ascii_entry();

	printf("test_edit: %d checks, %d failures\n", checks, failures);

	return failures ? 1 : 0;

}
