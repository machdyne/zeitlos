/*
 * Host test for sw/os/msg.c: what happens to the messages of a process
 * that has died (k_msg_release_pid()).
 *
 *   cc -std=gnu99 -O2 -Wall -Wno-int-to-void-pointer-cast \
 *      -Wno-void-pointer-to-int-cast -I sw/common -o /tmp/t \
 *      sw/os/tests/test_msg.c sw/common/zobj.c && /tmp/t
 *
 * (msg.c keeps pointers in uint32_t, as the kernel does; on a 64-bit
 * host that only warns, and nothing here depends on it.) Clean under
 * -fsanitize=address,undefined.
 *
 * msg.c is included, not linked, so the test sees its mailboxes. The
 * one thing a Mach-O host will not take is its section(".bss")
 * attribute, which only matters for where the kernel image puts them.
 *
 * Why this exists: messages carry pointers into the SENDER's memory,
 * resolved when they are read. Closing irc with its socket open left
 * its QUIT (a blob in irc's heap) in net's mailbox after irc had been
 * reaped; net's read translated it against base 0, into an address
 * nothing on the bus decodes, and the machine stopped. After the reap
 * no queued message may point at the dead process's memory, and a
 * CLOSE must still arrive.
 *
 * 1. pointers      a dead sender's STR/BLOB/LIST/MAP arrive as Z_NONE,
 *                  envelope (subject, tag, order) unchanged
 * 2. scalars       its UINT32/INT32/NONE messages are untouched
 * 3. others        other senders' pointers are untouched
 * 4. wrap          the same across a ring that wraps
 * 5. own mailbox   the dead process's own mailbox is emptied
 * 6. read          k_msg_read() then hands out Z_NONE without
 *                  translating anything
 * 7. edges         pid 0 (the kernel) and out-of-range pids: no-op
 * 8. pop_match     z_mailbox_pop_match() takes one message from any
 *                  position, across the wrap, and keeps the rest in order
 * 9. coalesce      a pointer move merges into the newest queued move
 *                  from the same sender and window with the same
 *                  buttons -- never into an older one, a press or a
 *                  release, and never past a key
 * 11. create map   zwin's 7-key Z_WM_CREATE_WINDOW map resolves whole
 *                  (14 of 16 items); 9 pairs are refused, as Z_NONE
 * 10. reserve      input cannot take the last Z_MAILBOX_RESERVE slots;
 *                  a reply still fits there (the ask/read/backup 15 s
 *                  stalls: keys filled the mailbox, fs's reply was lost)
 *
 * Checked against broken versions of k_msg_release_pid(): scanning
 * from slot 0 instead of the ring's head fails 25 checks, not emptying
 * the dead one's mailbox 8, releasing pid 0 one. With the pointers left
 * alone (or the function empty), 6 does what the board did: the read
 * translates the QUIT's 0x8002d318 against base 0 and dereferences
 * 0x0002d318 in z_resolve_obj() -- here a SIGSEGV, there a bus cycle
 * nothing ever acks.
 */
// kernel.h's maskirq() is the PicoRV32 instruction: renamed out of
// the way while the headers are read, and replaced by a no-op.
#define maskirq maskirq_picorv32
#include "../kernel.h"
#undef maskirq
static uint32_t maskirq(uint32_t m) { (void)m; return 0; }

#define section(x) used
#include "../msg.c"
#undef section

#include <stdio.h>
#include <string.h>

volatile uint32_t z_pid;
volatile z_proc z_procs[Z_PROCS_MAX];
volatile uint32_t z_kernel_ticks;
void k_proc_unblock(uint32_t pid) { (void)pid; }
// msg.c's z_msg_recv_match() (for kernel clients of fs, kfs.c) calls
// kernel.c's k_msg_recv(); not exercised here.
z_obj_t *k_msg_recv(z_obj_t *args) { (void)args; return &z_fail; }

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; \
	printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void reset(void) {
	memset((void *)z_mailboxes, 0, sizeof(z_mailboxes));
	memset((void *)z_procs, 0, sizeof(z_procs));
}

static void put(uint32_t to, uint32_t from, uint32_t subject, uint32_t tag, z_type_t type) {
	z_msg_envelope_t e;
	memset(&e, 0, sizeof(e));
	e.to = to; e.from = from; e.subject = subject; e.tag = tag;
	e.obj.type = type;
	if (type != Z_NONE && type != Z_UINT32 && type != Z_INT32)
		e.obj.val.ptr = (void *)(uintptr_t)0x8002d318u;	// the QUIT's address on the board
	else
		e.obj.val.uint32 = 1234;
	if (z_mailbox_push(to, &e) != Z_OK) { printf("push failed\n"); fails++; }
}

static z_msg_envelope_t at(uint32_t pid, uint32_t i) {
	z_msg_envelope_t e;
	memcpy(&e, (const void *)&z_mailboxes[pid].msgs[(z_mailboxes[pid].head + i) % Z_MAILBOX_DEPTH], sizeof(e));
	return e;
}

// The shape of the irc case: net (2) holds irc's (5) DATA and CLOSE,
// plus traffic from others; irc's own mailbox holds an ack from net.
static void fill(void) {
	put(2, 5, 203, 4, Z_BLOB);      // Z_PORT_DATA "QUIT :Zeitlos"
	put(2, 3, 203, 9, Z_BLOB);      // someone else's DATA
	put(2, 5, 204, 4, Z_NONE);      // Z_PORT_CLOSE
	put(2, 5, 300, 1, Z_UINT32);
	put(2, 5, 301, 1, Z_STR);
	put(2, 5, 302, 1, Z_LIST);
	put(2, 5, 303, 1, Z_MAP);
	put(2, 5, 304, 1, Z_INT32);
	put(1, 5, 400, 1, Z_STR);       // a window title to wm
	put(1, 6, 401, 1, Z_STR);
	put(5, 2, 205, 4, Z_NONE);      // net's ack, to irc
	put(5, 2, 206, 4, Z_BLOB);
	z_procs[5].size = 0x31000;       // what the reap leaves; base is 0
}

static void expect_released(const char *what) {
	static const struct { uint32_t subject, tag; z_type_t type; } net[] = {
		{ 203, 4, Z_NONE }, { 203, 9, Z_BLOB }, { 204, 4, Z_NONE }, { 300, 1, Z_UINT32 },
		{ 301, 1, Z_NONE }, { 302, 1, Z_NONE }, { 303, 1, Z_NONE }, { 304, 1, Z_INT32 },
	};
	CHECK(z_mailboxes[2].count == 8, "%s: net's mailbox holds %u, not 8", what,
		(unsigned)z_mailboxes[2].count);
	for (uint32_t i = 0; i < 8; i++) {
		z_msg_envelope_t e = at(2, i);
		CHECK(e.subject == net[i].subject && e.tag == net[i].tag,
			"%s: net #%u is %u/%u, not %u/%u", what, (unsigned)i, (unsigned)e.subject,
			(unsigned)e.tag, (unsigned)net[i].subject, (unsigned)net[i].tag);
		CHECK(e.obj.type == net[i].type, "%s: net #%u has type %d, not %d", what,
			(unsigned)i, (int)e.obj.type, (int)net[i].type);
	}
	CHECK(at(2, 3).obj.val.uint32 == 1234, "%s: a scalar payload changed", what);
	CHECK(at(2, 1).obj.val.ptr == (void *)(uintptr_t)0x8002d318u,
		"%s: another sender's pointer changed", what);
	CHECK(z_mailboxes[1].count == 2 && at(1, 0).obj.type == Z_NONE && at(1, 1).obj.type == Z_STR,
		"%s: wm's mailbox wrong", what);
	CHECK(z_mailboxes[5].count == 0 && z_mailboxes[5].head == z_mailboxes[5].tail,
		"%s: the dead process's own mailbox still holds %u", what, (unsigned)z_mailboxes[5].count);
}

// 8. z_mailbox_pop_match(): the one message with a subject and tag
// comes out from wherever it is; the rest keep their order. Every
// position, every ring start (so the gap closes across the wrap).
static void test_pop_match(void) {
	for (uint32_t start = 0; start < Z_MAILBOX_DEPTH; start += 3) {
		for (uint32_t n = 1; n <= 9; n++) {
			for (uint32_t pick = 0; pick < n; pick++) {
				reset();
				z_mailboxes[2].head = z_mailboxes[2].tail = start;
				for (uint32_t i = 0; i < n; i++) put(2, 3, 500 + i, 7, Z_UINT32);
				z_msg_envelope_t e;
				CHECK(z_mailbox_pop_match(2, 500 + pick, 7, &e) == Z_OK &&
					e.subject == 500 + pick, "match start %u n %u pick %u",
					(unsigned)start, (unsigned)n, (unsigned)pick);
				CHECK(z_mailboxes[2].count == n - 1, "count after match");
				for (uint32_t i = 0, want = 500; i < n - 1; i++, want++) {
					if (want == 500 + pick) want++;
					CHECK(z_mailbox_pop(2, &e) == Z_OK && e.subject == want,
						"order: start %u n %u pick %u: #%u is %u, want %u",
						(unsigned)start, (unsigned)n, (unsigned)pick, (unsigned)i,
						(unsigned)e.subject, (unsigned)want);
				}
				CHECK(z_mailbox_pop(2, &e) != Z_OK, "left over");
			}
		}
	}
	reset();
	put(2, 3, 600, 1, Z_UINT32);
	z_msg_envelope_t e;
	CHECK(z_mailbox_pop_match(2, 600, 2, &e) != Z_OK, "tag must match too");
	CHECK(z_mailbox_pop_match(2, 601, 1, &e) != Z_OK, "subject must match too");
	CHECK(z_mailboxes[2].count == 1, "a miss takes nothing");
}


static uint32_t mv(uint32_t x, uint32_t y, uint32_t btn) {
	return Z_WM_PACK_MOUSE(x, y, btn, 1);
}

static z_rv put_u(uint32_t to, uint32_t from, uint32_t subject, uint32_t tag, uint32_t v) {
	z_msg_envelope_t e;
	memset(&e, 0, sizeof(e));
	e.to = to; e.from = from; e.subject = subject; e.tag = tag;
	e.obj.type = Z_UINT32;
	e.obj.val.uint32 = v;
	return z_mailbox_push(to, &e);
}

static void test_coalesce(void) {
	reset();
	// a run of moves while the app is busy: one slot, the last position
	for (uint32_t i = 0; i < 100; i++)
		CHECK(put_u(4, 1, Z_WM_MOUSE, 3, mv(i, i, 0)) == Z_OK, "move %u refused", (unsigned)i);
	CHECK(z_mailboxes[4].count == 1, "100 moves took %u slots", (unsigned)z_mailboxes[4].count);
	CHECK(at(4, 0).obj.val.uint32 == mv(99, 99, 0), "the merged move is not the last one");

	// a press is never merged into a move, nor a release into a drag
	put_u(4, 1, Z_WM_MOUSE, 3, mv(5, 5, 1));
	put_u(4, 1, Z_WM_MOUSE, 3, mv(6, 6, 1));	// drag: merges into the press's slot...
	CHECK(z_mailboxes[4].count == 2, "press+drag: %u slots", (unsigned)z_mailboxes[4].count);
	CHECK(Z_WM_UNPACK_MOUSE_BUTTONS(at(4, 1).obj.val.uint32) == 1, "the press was lost");
	put_u(4, 1, Z_WM_MOUSE, 3, mv(7, 7, 0));	// ...the release does not
	CHECK(z_mailboxes[4].count == 3, "release merged away");

	// a key in between stops merging, so no move overtakes it
	put_u(4, 1, Z_WM_KEY, 3, 0x1234);
	put_u(4, 1, Z_WM_MOUSE, 3, mv(8, 8, 0));
	CHECK(z_mailboxes[4].count == 5, "a move merged across a key");

	// another window, another sender: separate
	put_u(4, 1, Z_WM_MOUSE, 9, mv(1, 1, 0));
	put_u(4, 2, Z_WM_MOUSE, 9, mv(1, 1, 0));
	CHECK(z_mailboxes[4].count == 7, "moves to another window or from another sender merged");
	reset();
}

static void test_reserve(void) {
	reset();
	uint32_t limit = Z_MAILBOX_DEPTH - Z_MAILBOX_RESERVE, n = 0;
	for (uint32_t i = 0; i < Z_MAILBOX_DEPTH + 10; i++)
		if (put_u(4, 1, Z_WM_KEY, 3, i) == Z_OK) n++;
	CHECK(n == limit, "keys took %u slots, reserve leaves %u", (unsigned)n, (unsigned)limit);
	CHECK(put_u(4, 1, Z_WM_WHEEL, 3, 1) == Z_FAIL, "a wheel notch took the reserve");
	// the reply the app is blocked on still arrives
	CHECK(put_u(4, 2, 1000, 77, 0) == Z_OK, "fs's reply was refused with keys queued");
	// other mail fills the rest, then the mailbox is full
	for (uint32_t i = 1; i < Z_MAILBOX_RESERVE; i++)
		CHECK(put_u(4, 2, 1000, 77 + i, 0) == Z_OK, "reply %u refused", (unsigned)i);
	CHECK(put_u(4, 2, 1000, 99, 0) == Z_FAIL, "pushed past the depth");
	// and the keys are all there, in order
	for (uint32_t i = 0; i < limit; i++)
		CHECK(at(4, i).obj.val.uint32 == i, "key %u out of order", (unsigned)i);
	reset();
}

// 11. create map: wm's Z_WM_CREATE_WINDOW from zwin.c, at its seven
// keys (title, w, h, x, y, flags, min), resolves whole within the
// message's scratch -- and nine pairs, past Z_MSG_MAX_ITEMS, do not.
// An over-budget map arrives as Z_NONE, and the window is not made.
static void test_create_map(void) {
	static const char *names[9] = { "title", "w", "h", "x", "y", "flags", "min", "k8", "k9" };
	static z_obj_t keys[9], vals[9];
	static z_obj_table_t tbl;
	for (int n = 7; n <= 9; n += 2) {
		for (int i = 0; i < n; i++) {
			keys[i].type = Z_STR; keys[i].val.str = (char *)names[i];
			vals[i].type = Z_UINT32; vals[i].val.uint32 = (uint32_t)(100 + i);
		}
		tbl.len = (uint32_t)n; tbl.a = keys; tbl.b = vals;
		z_obj_t obj;
		obj.type = Z_MAP; obj.val.ptr = &tbl;
		z_obj_table_t tables[Z_MSG_MAX_TABLES];
		z_obj_t items[Z_MSG_MAX_ITEMS];
		z_blob_t blobs[Z_MSG_MAX_BLOBS];
		uint32_t tc = 0, ic = 0, bc = 0;
		z_rv rv = z_resolve_obj(0, &obj, tables, &tc, items, &ic, blobs, &bc);
		if (n == 7) {
			CHECK(rv == Z_OK && obj.type == Z_MAP, "a 7-key create map did not resolve");
			z_obj_table_t *t = (z_obj_table_t *)obj.val.ptr;
			CHECK(t && t->len == 7 && t->b[6].val.uint32 == 106 &&
				!strcmp(t->a[6].val.str, "min"), "the min key did not arrive");
			CHECK(ic == 14, "7 pairs took %u of %d items", (unsigned)ic, Z_MSG_MAX_ITEMS);
		} else {
			CHECK(rv != Z_OK && obj.type == Z_NONE, "a 9-key map past the budget resolved");
		}
	}
}

int main(void) {

	setvbuf(stdout, NULL, _IONBF, 0);	// a crash in 6 must not eat the output

	test_pop_match();

	// 1-3, 5
	reset();
	fill();
	k_msg_release_pid(5);
	expect_released("basic");

	// 4: the same with every ring started near its end, so the scan wraps
	for (uint32_t start = 1; start < Z_MAILBOX_DEPTH; start += 5) {
		char what[32];
		reset();
		for (uint32_t p = 0; p < Z_PROCS_MAX; p++)
			z_mailboxes[p].head = z_mailboxes[p].tail = Z_MAILBOX_DEPTH - start;
		fill();
		k_msg_release_pid(5);
		snprintf(what, sizeof(what), "wrap %u", (unsigned)start);
		expect_released(what);
	}

	// 4b: a full mailbox of the dead one's pointers
	reset();
	for (uint32_t i = 0; i < Z_MAILBOX_DEPTH; i++) put(2, 5, 203, i, Z_BLOB);
	k_msg_release_pid(5);
	for (uint32_t i = 0; i < Z_MAILBOX_DEPTH; i++)
		CHECK(at(2, i).obj.type == Z_NONE && at(2, i).tag == i, "full: #%u", (unsigned)i);

	// 6: net reads what irc left: the envelope, and nothing to translate
	printf("6. read after release (a crash here is the bug)\n");
	reset();
	fill();
	k_msg_release_pid(5);
	z_pid = 2;
	{
		z_msg_t m;
		memset(&m, 0, sizeof(m));
		CHECK(k_msg_read((z_obj_t *)&m) == &z_ok, "read failed");
		CHECK(m.from == 5 && m.subject == 203 && m.tag == 4 && m.obj.type == Z_NONE,
			"read: from %u subject %u tag %u type %d", (unsigned)m.from,
			(unsigned)m.subject, (unsigned)m.tag, (int)m.obj.type);
		CHECK(z_blob_len(&m.obj) == 0 && z_blob_data(&m.obj) == NULL,
			"read: an empty DATA still has a blob");
	}

	// 7: the kernel's own messages, and pids that do not exist
	reset();
	put(2, 0, 500, 1, Z_STR);
	k_msg_release_pid(0);
	k_msg_release_pid(Z_PROCS_MAX);
	k_msg_release_pid(0xFFFFFFFFu);
	CHECK(at(2, 0).obj.type == Z_STR && z_mailboxes[2].count == 1, "pid 0 was released");

	// Payload pointers: a window address is the sender's block; an
	// address in the sender's arena is physical already and stays so
	// (slide writing a deck from its pool reached fs as 0x2000xxxx)
	z_procs[7].base = 0x40500000u;
	z_procs[7].size = 0x40000u;
	z_procs[7].arena_base = 0x40e00000u;
	z_procs[7].arena_size = 0x600000u;
	CHECK((uint32_t)(uintptr_t)z_translate(7, (void *)(uintptr_t)0x80001000u) == 0x40501000u,
		"a window address goes to the block");
	CHECK((uint32_t)(uintptr_t)z_translate(7, (void *)(uintptr_t)0x40e01bb0u) == 0x40e01bb0u,
		"an arena address stays as it is");
	CHECK((uint32_t)(uintptr_t)z_translate(7, (void *)(uintptr_t)0x41400000u) != 0x41400000u,
		"just past the arena is not passed through");
	z_procs[7].arena_size = 0;
	z_procs[7].base = 0;
	z_procs[7].size = 0;

	test_coalesce();
	test_reserve();
	test_create_map();

	printf("test_msg: %d checks, %d failed\n", checks, fails);
	return fails != 0;

}
