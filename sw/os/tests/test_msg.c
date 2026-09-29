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

int main(void) {

	setvbuf(stdout, NULL, _IONBF, 0);	// a crash in 6 must not eat the output

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

	printf("test_msg: %d checks, %d failed\n", checks, fails);
	return fails != 0;

}
