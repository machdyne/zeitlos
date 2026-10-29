/*
 * Host test for text.c's editing core: the gap buffer, the line table,
 * the arena, undo and redo, and the clipboard. Host build: `make test`.
 *
 * Builds the REAL text.c -- #included below, its main() renamed --
 * over real zwin/zgfx/zwidget (drawing into mapped VRAM, zrender.h)
 * and a syscall trampoline that answers one call for real: the arena.
 *
 * The arena is the point. text keeps a document past 32 KB, its undo
 * rings once they outgrow .bss, and a copy of a large clipboard in one
 * block that the kernel may MOVE on any resize (zeitlos.h,
 * z_arena_set()). On the board it moves only sometimes, so a pointer
 * held across a resize works by luck until the day it does not. Here
 * it moves on every resize -- the block alternates between two fixed
 * regions -- and the region left behind is made unreadable, so a stale
 * pointer faults on its next use.
 *
 * What it checks:
 *
 *   1. random edits   inserts, deletes and large pastes at random
 *                     places, with undo and redo between: after every
 *                     step the document is byte for byte what a plain
 *                     model says, and the line table is sane
 *   2. undo depth     a change far larger than the starting rings
 *                     (a 300 KB delete) undoes and redoes whole
 *   3. no arena       the same edits with the kernel refusing an arena:
 *                     the fixed .bss rings, history shortened, never
 *                     wrong
 *   4. clipboard      copy, cut and paste of selections over 4 KB: the
 *                     whole selection comes back, other apps get the
 *                     first 4 KB, and a cut that could not be pasted
 *                     back or undone is refused, deleting nothing
 *
 * Needs -no-pie and vm.mmap_min_addr=0 (sw/common/tests/ztramp.h);
 * exits 77 (skip) without them.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/mman.h>

#include "../../../common/zeitlos.h"
#include "../../../common/zproc.h"
#include "../../../common/zwm.h"
#include "../../../common/tests/zrender.h"
#include "../../../common/zwin.h"
#include "../../../common/zdialog.h"

// -- the clipboard and dialogs, faked by name --
//
// text.c's calls go to these; zwin.c's own are left as they are.

static char fclip[Z_WM_CLIP_MAX];
static int fclip_len = -1;			// -1: empty
static char last_dialog[256];
static int dialogs;

static void fake_clip_set(const char *t, int n) {
	int i = 0;		// what zwin.c's z_clip_set() keeps: up to MAX - 1, up to a NUL
	for (; t && i < Z_WM_CLIP_MAX - 1 && (n < 0 || i < n) && t[i]; i++) fclip[i] = t[i];
	fclip[i] = 0;
	fclip_len = i;
}

static int fake_clip_get(char *out, int outlen) {
	if (fclip_len < 0 || outlen <= 0) return 0;
	int n = fclip_len < outlen - 1 ? fclip_len : outlen - 1;
	memcpy(out, fclip, (size_t)n);
	out[n] = 0;
	return n;
}

static int fake_confirm(const z_dialog_ctx_t *ctx, const char *title, const char *msg, int b) {
	(void)ctx; (void)b;
	snprintf(last_dialog, sizeof last_dialog, "%s: %s", title, msg);
	dialogs++;
	return 1;
}

#define z_clip_set		fake_clip_set
#define z_clip_get		fake_clip_get
#define z_dialog_confirm	fake_confirm
#define main			text_main
#define TEXT_HOST_TEST
#include "../text.c"
#undef main
#undef z_clip_set
#undef z_clip_get
#undef z_dialog_confirm

bool z_fb_hw_blit_mem_available(void) { return true; }

// zgfx.c pieces zrender.h does not stand in for (as tests/utf8.c).
int z_fb_scroll_debug, z_fb_scroll_dbg_armed, z_fb_scroll_align;
void z_fb_draw_icon(int x, int y, int icon_id, int fg, int bg, const z_clip_t *clip) {
	(void)x; (void)y; (void)icon_id; (void)fg; (void)bg; (void)clip;
}

// -- the fake kernel --

#define REGION_A	((void *)0x10000000)
#define REGION_B	((void *)0x18000000)
#define REGION_LEN	(64u << 20)

static uint8_t *ar_base;			// NULL: no arena
static uint32_t ar_size;
static uint32_t ar_limit = REGION_LEN;	// what the "machine" has
static bool ar_refuse;				// E_PERM: a board without arenas
static long arena_moved;

static void arena_call(z_arena_args_t *a) {
	if (ar_refuse) { a->base = 0; a->size = 0; a->result = Z_ARENA_E_PERM; return; }
	if (a->size == Z_ARENA_QUERY) {
		a->base = (uint32_t)(uintptr_t)ar_base;
		a->size = ar_base ? ar_size : 0;
		a->result = Z_ARENA_OK;
		return;
	}
	if (a->size > ar_limit) {
		a->base = (uint32_t)(uintptr_t)ar_base;
		a->size = ar_base ? ar_size : 0;
		a->result = Z_ARENA_E_NOMEM;
		return;
	}
	if (a->size == 0) {
		if (ar_base) mprotect(ar_base, REGION_LEN, PROT_NONE);
		ar_base = NULL;
		ar_size = 0;
		a->base = 0;
		a->result = Z_ARENA_OK;
		return;
	}
	// Every resize moves it, contents kept up to the smaller size, and
	// what it leaves is made unreadable: a pointer held across the
	// resize faults on its next use, instead of reading what used to be
	// there and working by luck.
	uint8_t *to = (ar_base == (uint8_t *)REGION_A) ? REGION_B : REGION_A;
	mprotect(to, REGION_LEN, PROT_READ | PROT_WRITE);
	memset(to, 0x5A, a->size);
	if (ar_base) {
		memcpy(to, ar_base, ar_size < a->size ? ar_size : a->size);
		mprotect(ar_base, REGION_LEN, PROT_NONE);
		arena_moved++;
	}
	ar_base = to;
	ar_size = a->size;
	a->base = (uint32_t)(uintptr_t)ar_base;
	a->result = Z_ARENA_OK;
}

static z_obj_t t_ok, t_fail;

static uint32_t *tramp(uint32_t id, uint32_t *args, uint32_t b) {
	(void)b;
	if (id == Z_SYS_ARENA) { arena_call((z_arena_args_t *)args); return (uint32_t *)&t_ok; }
	if (id == Z_SYS_MSG_READ || id == Z_SYS_MSG_RECV) return (uint32_t *)&t_fail;
	return (uint32_t *)&t_ok;
}

static bool machine_up(void) {
	if ((uintptr_t)(void *)tramp > 0xFFFFFFFFu) return false;
	void *page = mmap((void *)0, 4096, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if (page == MAP_FAILED) return false;
	t_ok.type = Z_UINT32; t_ok.val.uint32 = Z_OK;
	t_fail.type = Z_UINT32; t_fail.val.uint32 = Z_FAIL;
	*(volatile uint32_t *)0x0000000c = (uint32_t)(uintptr_t)tramp;
	if (mmap(REGION_A, REGION_LEN, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == MAP_FAILED) return false;
	if (mmap(REGION_B, REGION_LEN, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == MAP_FAILED) return false;
	if (!z_render_open(&win, WIN_W, WIN_H)) return false;
	z_scrollbar_init(&sbar, &win, Z_SB_VERT);
	return true;
}


// -- checks --

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; \
	printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// The document as one string, read through the gap.
static char *doc_copy(int *n) {
	char *s = malloc((size_t)len + 1);
	for (int i = 0; i < len; i++) s[i] = B(i);
	s[len] = 0;
	*n = len;
	return s;
}

static bool doc_is(const char *m, int mn) {
	if (mn != len) return false;
	for (int i = 0; i < len; i++) if (B(i) != m[i]) return false;
	return true;
}

// Every display line starts inside the text, in order, the first at 0,
// and every paragraph's first byte starts one.
static bool lines_sane(char *why, size_t wn) {
	if (nlines < 1) { snprintf(why, wn, "nlines %d", nlines); return false; }
	if (LO(0) != 0) { snprintf(why, wn, "line 0 at %d", LO(0)); return false; }
	int prev = 0, l = 1;
	for (int i = 1; i < nlines; i++) {
		int o = LO(i);
		if (o < prev || o > len) { snprintf(why, wn, "line %d at %d after %d (len %d)", i, o, prev, len); return false; }
		prev = o;
	}
	for (int i = 0; i < len; i++) {
		if (B(i) != '\n' || i + 1 > len) continue;
		while (l < nlines && LO(l) < i + 1) l++;
		if (l < nlines && LO(l) != i + 1) {
			snprintf(why, wn, "no line starts after the newline at %d", i);
			return false;
		}
	}
	return true;
}


// A board without arenas. text gives back what it holds first (a real
// kernel never takes an arena back, and a shrink never fails), then
// every call is refused.
static void no_arenas(bool on) {
	if (on) {
		clip_drop();
		buf_clear();
		CHECK(ar_base == NULL, "text gave its arena back (%u bytes held)", (unsigned)ar_size);
	}
	ar_refuse = on;
}

// -- the model: snapshots of the document, one per group --

typedef struct { char *s; int n; } snap_t;
#define MAXSNAP 4096
static snap_t past[MAXSNAP], futr[MAXSNAP];
static int npast, nfutr;

static void model_reset(void) {
	for (int i = 0; i < npast; i++) free(past[i].s);
	for (int i = 0; i < nfutr; i++) free(futr[i].s);
	npast = nfutr = 0;
}

static void model_before_edit(void) {
	for (int i = 0; i < nfutr; i++) free(futr[i].s);
	nfutr = 0;
	if (npast == MAXSNAP) { free(past[0].s); memmove(past, past + 1, sizeof(past[0]) * (MAXSNAP - 1)); npast--; }
	past[npast].s = doc_copy(&past[npast].n);
	npast++;
}

static uint32_t rng = 12345;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static void fill_random(char *s, int n) {
	static const char al[] = "abc defg\nhij klm nop\n\nqrs tuv wxyz ";
	for (int i = 0; i < n; i++) s[i] = al[rnd() % (sizeof(al) - 1)];
}

static void start_doc(void) {
	model_reset();
	buf_clear();
	layout();
	fclip_len = -1;
}

static void op_insert(int n) {
	model_before_edit();
	char *s = malloc((size_t)n);
	fill_random(s, n);
	int at = len ? (int)(rnd() % (uint32_t)(len + 1)) : 0;
	cursor = at;
	undo_begin(UK_OTHER, at);
	if (n > gl && !doc_room(n)) { free(s); free(past[--npast].s); return; }
	edit_insert(at, s, n);
	cursor = at + n;
	undo_break();
	after_edit(para_line_at(at));
	free(s);
}

static void op_delete(int n) {
	if (!len) return;
	model_before_edit();
	int a = (int)(rnd() % (uint32_t)len);
	int b = a + n > len ? len : a + n;
	undo_begin(UK_OTHER, a);
	int para = para_line_at(a);
	edit_delete(a, b);
	cursor = a;
	undo_break();
	after_edit(para);
}

// One undo or redo, checked against the model. A history that ran out
// (the oldest edits forgotten) is not a failure; a document that comes
// back different from what it was is.
static void op_undo(bool redo, const char *ctx) {
	bool could = redo ? z_undo_can_redo(&undo) : z_undo_can_undo(&undo);
	int pn;
	char *pre = doc_copy(&pn);			// what an undo takes back, a redo puts back
	do_undo(redo);
	if (!could) { free(pre); return; }
	snap_t *want = redo ? (nfutr ? &futr[nfutr - 1] : NULL) : (npast ? &past[npast - 1] : NULL);
	if (!want) { free(pre); return; }
	if (!doc_is(want->s, want->n)) {
		free(pre);
		// The oldest group left can have lost its first records to a
		// full ring; undoing it reaches the end of the history
		// without reaching the model's state. Not wrong: forgotten.
		if (!redo && !z_undo_can_undo(&undo)) {
			model_reset();
			return;
		}
		CHECK(false, "%s: %s gave %d bytes, wanted %d", ctx, redo ? "redo" : "undo", len, want->n);
		return;
	}
	if (redo) {
		free(futr[nfutr - 1].s);
		nfutr--;
		past[npast].s = pre; past[npast].n = pn; npast++;
	} else {
		free(past[npast - 1].s);
		npast--;
		futr[nfutr].s = pre; futr[nfutr].n = pn; nfutr++;
	}
}

static void random_run(const char *ctx, int steps, int big_every) {
	char why[160];
	start_doc();
	char *model; int mn;
	for (int i = 0; i < steps; i++) {
		uint32_t r = rnd() % 100;
		if (big_every && i % big_every == big_every - 1) op_insert(20000 + (int)(rnd() % 90000));
		else if (r < 35) op_insert(1 + (int)(rnd() % 60));
		else if (r < 45) op_insert(200 + (int)(rnd() % 4000));
		else if (r < 60) op_delete(1 + (int)(rnd() % 40));
		else if (r < 66) op_delete(500 + (int)(rnd() % 30000));
		else if (r < 85) op_undo(false, ctx);
		else op_undo(true, ctx);
		if (i % 7 == 0 && !lines_sane(why, sizeof why)) {
			CHECK(false, "%s: step %d: line table: %s", ctx, i, why);
			break;
		}
		if (i % 97 == 0 && !layout_check()) {
			CHECK(false, "%s: step %d: line table is not what a full rewrap builds", ctx, i);
			break;
		}
		if (fails > 20) break;
	}
	// Undo everything that is left, then redo it all: every state on the
	// way must be the model's.
	while (z_undo_can_undo(&undo) && npast) op_undo(false, ctx);
	while (z_undo_can_redo(&undo) && nfutr) op_undo(true, ctx);
	model = doc_copy(&mn);
	CHECK(lines_sane(why, sizeof why), "%s: line table at the end: %s", ctx, why);
	CHECK(layout_check(), "%s: line table at the end is a full rewrap's", ctx);
	free(model);
}


// The arena moves on every resize here, but text resizes only as a
// document grows. So: many short documents, each grown past sbuf in
// steps (go_big, then doubling), edited, undone and redone back down.
static void move_stress(void) {
	long m0 = arena_moved;
	for (int round = 0; round < 40; round++) {
		start_doc();
		for (int i = 0; i < 12; i++) op_insert(4000 + (int)(rnd() % 12000));
		for (int i = 0; i < 30; i++) {
			uint32_t r = rnd() % 4;
			if (r == 0) op_insert(1 + (int)(rnd() % 3000));
			else if (r == 1) op_delete(1 + (int)(rnd() % 8000));
			else op_undo(r == 3, "move stress");
		}
		while (z_undo_can_undo(&undo) && npast) op_undo(false, "move stress");
		while (z_undo_can_redo(&undo) && nfutr) op_undo(true, "move stress");
		CHECK(layout_check(), "move stress %d: line table is a full rewrap's", round);
		if (fails > 20) break;
	}
	CHECK(arena_moved - m0 > 100, "the arena moved %ld times under the stress", arena_moved - m0);
}

// 2: one change far past the rings' starting size.
static void huge_change(void) {
	start_doc();
	int n = 300000;
	char *s = malloc((size_t)n);
	fill_random(s, n);
	undo_begin(UK_OTHER, 0);
	CHECK(doc_room(n), "room for 300 KB");
	edit_insert(0, s, n);
	undo_break();
	after_edit(0);
	z_undo_saved(&undo);
	char *before; int bn;
	before = doc_copy(&bn);
	// delete it nearly all, one change
	undo_begin(UK_OTHER, 10);
	edit_delete(10, n - 10);
	undo_break();
	after_edit(0);
	CHECK(len == 20, "deleted to 20 bytes (%d)", len);
	CHECK(z_undo_can_undo(&undo), "a 300 KB delete can be undone");
	do_undo(false);
	CHECK(doc_is(before, bn), "undo brought all 300 KB back");
	CHECK(z_undo_at_saved(&undo), "and is at the saved point");
	do_undo(true);
	CHECK(len == 20, "redo deleted it again (%d)", len);
	do_undo(false);
	CHECK(doc_is(before, bn), "and undo again");
	free(before);
	free(s);
}

// 4: the clipboard.
static void select_range(int a, int b) { sel_anchor = a; cursor = b; }

static void clipboard(void) {
	char *orig; int on;

	// a 10 KB copy, pasted back whole
	start_doc();
	op_insert(30000);
	orig = doc_copy(&on);
	select_range(1000, 11000);
	do_copy();
	CHECK(fclip_len == Z_WM_CLIP_MAX - 1 || fclip_len == Z_WM_CLIP_MAX - 2 ||
		fclip_len == Z_WM_CLIP_MAX - 3 || fclip_len == Z_WM_CLIP_MAX - 4,
		"other apps get the first 4 KB (%d)", fclip_len);
	CHECK(memcmp(fclip, orig + 1000, (size_t)fclip_len) == 0, "and it is the selection's start");
	sel_anchor = -1;
	cursor = len;
	dialogs = 0;
	do_paste();
	CHECK(len == on + 10000, "the paste is the whole 10 KB (%d more)", len - on);
	int bad = -1;
	for (int i = 0; i < 10000 && bad < 0; i++) if (B(on + i) != orig[1000 + i]) bad = i;
	CHECK(bad < 0, "byte for byte (first difference at %d of the paste)", bad);
	CHECK(dialogs == 0, "and says nothing (%s)", last_dialog);
	free(orig);

	// the copy survives the document growing past it: every resize
	// moves the rings and the copy above the text (the bug: the copy
	// was moved after the rings, which had already been written over it)
	{
		char *want = malloc(12000);
		select_range(500, 12500);
		for (int i = 0; i < 12000; i++) want[i] = B(500 + i);
		do_copy();
		sel_anchor = -1;
		long m0 = arena_moved;
		for (int i = 0; i < 8; i++) op_insert(30000);
		CHECK(arena_moved > m0, "the arena moved while the copy was held");
		int at = len;
		cursor = at;
		do_paste();
		int bad2 = -1;
		for (int i = 0; i < 12000 && bad2 < 0; i++) if (B(at + i) != want[i]) bad2 = i;
		CHECK(len == at + 12000 && bad2 < 0, "the copy came through the moves whole (first difference %d)", bad2);
		free(want);
	}

	// another app copies something else: its text, not ours
	fake_clip_set("other", -1);
	int l0 = len;
	cursor = 0;
	sel_anchor = -1;
	do_paste();
	CHECK(len == l0 + 5 && B(0) == 'o' && B(4) == 'r', "another app's copy pastes as itself");

	// a 20 KB cut: gone, pasted back, then undone twice
	start_doc();
	op_insert(40000);
	orig = doc_copy(&on);
	select_range(5000, 25000);
	do_cut();
	CHECK(len == on - 20000, "the cut took 20 KB (%d)", on - len);
	cursor = 5000;
	sel_anchor = -1;
	do_paste();
	CHECK(doc_is(orig, on), "cut and paste back is the document as it was");
	do_undo(false);
	CHECK(len == on - 20000, "undo the paste");
	do_undo(false);
	CHECK(doc_is(orig, on), "undo the cut");
	free(orig);

	// no arena: a cut over 4 KB cannot be pasted back, so it is refused
	no_arenas(true);
	start_doc();
	op_insert(20000);
	orig = doc_copy(&on);
	select_range(100, 9100);
	dialogs = 0;
	do_cut();
	CHECK(doc_is(orig, on), "with no arena, a 9 KB cut deletes nothing");
	CHECK(dialogs == 1, "and says why");
	// a small one still works
	select_range(100, 1100);
	do_cut();
	CHECK(len == on - 1000, "a 1 KB cut still cuts");
	// a large copy with no arena: says the paste will be the first 4 KB
	select_range(0, 9000);
	dialogs = 0;
	copy_cmd();
	CHECK(dialogs == 1, "a 9 KB copy with no arena says only 4 KB will paste");
	free(orig);
	no_arenas(false);
}

int main(void) {

	if (!machine_up()) {
		printf("test_edit: skipped (needs -no-pie and vm.mmap_min_addr=0)\n");
		return 77;
	}
	memset(sbuf, 0, sizeof(sbuf));
	memset(sline, 0, sizeof(sline));

	random_run("arena", 3000, 0);
	random_run("arena, big pastes", 1200, 40);
	huge_change();
	move_stress();

	no_arenas(true);
	random_run("no arena", 3000, 0);
	no_arenas(false);

	ar_limit = 256u << 10;		// a machine short of memory
	random_run("arena of 256 KB", 2000, 0);
	ar_limit = REGION_LEN;

	clipboard();

	printf("test_edit: %d checks, %d failed (%ld arena moves)\n", checks, fails, arena_moved);
	return fails != 0;
}
