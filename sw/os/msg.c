/*
 * Zeitlos OS
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Inter-process messaging.
 *
 * See ../common/zmsg.h for the design rationale. Summary: mailboxes
 * live in kernel memory (one fixed-depth ring per process slot), and
 * a message's payload is never copied -- z_msg_send() just queues the
 * envelope (which still points into the *sender's* own memory), and
 * z_msg_read() resolves those pointers lazily, only for the process
 * that actually reads the message.
 *
 * All of a process's own heap-allocated data lives inside that one
 * process's contiguous memory block (see mem.c/kernel.c), so a
 * pointer created by process P is always P.base + (vaddr - 0x80000000)
 * in physical terms. Physical addresses (i.e. anything below the
 * 0x8000_0000 MTU mirror window) aren't subject to address
 * translation, so the kernel can always read them directly regardless
 * of which process is currently scheduled. That's the trick this file
 * relies on throughout.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "kernel.h"
#include "msg.h"

// -- mailboxes --

typedef struct {
	z_msg_envelope_t	msgs[Z_MAILBOX_DEPTH];
	uint32_t		head;	// next slot to pop
	uint32_t		tail;	// next slot to push
	uint32_t		count;
} z_mailbox_t;

volatile __attribute__((section(".bss"))) z_mailbox_t z_mailboxes[Z_PROCS_MAX];

z_rv z_mailbox_is_empty(uint32_t pid) {
	return (z_mailboxes[pid].count == 0) ? Z_OK : Z_FAIL;
}

z_rv z_mailbox_is_full(uint32_t pid) {
	return (z_mailboxes[pid].count >= Z_MAILBOX_DEPTH) ? Z_OK : Z_FAIL;
}

z_rv z_mailbox_push(uint32_t pid, z_msg_envelope_t *msg) {

	// mask irqs so a scheduler swap can't interleave with another
	// process pushing/popping the same mailbox mid-update
	uint32_t old_mask = maskirq(0xFFFFFFFF);

	if (z_mailboxes[pid].count >= Z_MAILBOX_DEPTH) {
		maskirq(old_mask);
		return Z_FAIL;
	}

	z_mailboxes[pid].msgs[z_mailboxes[pid].tail] = *msg;
	z_mailboxes[pid].tail = (z_mailboxes[pid].tail + 1) % Z_MAILBOX_DEPTH;
	z_mailboxes[pid].count++;

	maskirq(old_mask);
	return Z_OK;

}

// Is this process's mailbox empty?
//
// Exists for k_proc_wait() (kernel.c), which must test this in the
// same syscall that sets Z_PROC_FLAG_BLOCKED -- see that function's
// comment on the lost-wakeup race. Reading count under maskirq for the
// same reason push/pop do: the mailbox is written from interrupt
// context.
bool z_mailbox_empty(uint32_t pid) {

	if (pid >= Z_PROCS_MAX) return true;

	uint32_t old_mask = maskirq(0xFFFFFFFF);
	bool empty = (z_mailboxes[pid].count == 0);
	maskirq(old_mask);

	return empty;

}

z_rv z_mailbox_pop(uint32_t pid, z_msg_envelope_t *msg) {

	uint32_t old_mask = maskirq(0xFFFFFFFF);

	if (z_mailboxes[pid].count == 0) {
		maskirq(old_mask);
		return Z_FAIL;
	}

	*msg = z_mailboxes[pid].msgs[z_mailboxes[pid].head];
	z_mailboxes[pid].head = (z_mailboxes[pid].head + 1) % Z_MAILBOX_DEPTH;
	z_mailboxes[pid].count--;

	maskirq(old_mask);
	return Z_OK;

}

// -- pointer resolution --

// convert a pointer that was created by process `pid` (i.e. it's
// expressed relative to that process's 0x8000_0000 mirror) into the
// physical address it actually refers to. the result is always below
// the mirror window, so it's dereferenceable directly, from any
// process's context, without going through the MTU.
//
// pid 0 (the kernel, including sh.c acting as pid 0) is a special
// case: it never runs through the 0x8000_0000 mirror at all -- it
// executes at, and allocates from, its own native/physical address
// space directly. applying the mirror-subtraction formula to a
// pointer that was never mirrored in the first place produces a wild,
// garbage address (a large unsigned underflow, since such a pointer
// is nowhere near 0x8000_0000) -- this was a real, crash-causing bug,
// only ever exercised once something first sent a message *from* the
// kernel to a regular process (previously, every message was
// app-to-app or app-to-wm; sh.c's tget/tput were the first
// kernel-to-app case).
static inline void *z_translate(uint32_t pid, void *vptr) {
	if (!vptr) return NULL;
	if (pid == 0) return vptr;
	return (void *)((uint32_t)vptr - 0x80000000 + z_procs[pid].base);
}

// resolve a z_obj_t that (possibly) belongs to process `from_pid` so
// it's safe for the *current* process to read. scalars are untouched.
// strings are pointed at the sender's bytes via a physical address --
// no copy. lists/maps have their structural nodes rebuilt into the
// caller-supplied scratch arrays (see zmsg.h); the leaf data inside
// them is left in place in the sender's memory. blobs work the same
// way as strings (leaf bytes never copied) but need their {len,data}
// header rebuilt into scratch too, since it's an indirect struct, not
// a single pointer. never mutates the sender's own memory.
//
// returns Z_FAIL (and resets obj to Z_NONE) if the scratch budget
// isn't big enough for the payload.
static z_rv z_resolve_obj(uint32_t from_pid, z_obj_t *obj,
	z_obj_table_t *tables, uint32_t *tcount,
	z_obj_t *items, uint32_t *icount,
	z_blob_t *blobs, uint32_t *bcount) {

	switch (obj->type) {

		case Z_NONE:
		case Z_RETVAL:
		case Z_UINT32:
		case Z_INT32:
		case Z_FLOAT32:
			return Z_OK;

		case Z_STR:
			obj->val.str = (char *)z_translate(from_pid, obj->val.str);
			return Z_OK;

		case Z_BLOB: {

			z_blob_t *src =
				(z_blob_t *)z_translate(from_pid, obj->val.ptr);

			if (!src) {
				obj->type = Z_NONE;
				return Z_OK;
			}

			if (*bcount >= Z_MSG_MAX_BLOBS) {
				obj->type = Z_NONE;
				return Z_FAIL;
			}

			z_blob_t *dst = &blobs[(*bcount)++];
			dst->len = src->len;
			dst->data = (uint8_t *)z_translate(from_pid, src->data);

			obj->val.ptr = dst;
			return Z_OK;

		}

		case Z_LIST:
		case Z_MAP: {

			z_obj_table_t *src =
				(z_obj_table_t *)z_translate(from_pid, obj->val.ptr);

			if (!src) {
				obj->type = Z_NONE;
				return Z_OK;
			}

			bool is_map = (obj->type == Z_MAP);
			uint32_t len = src->len;
			uint32_t need = len * (is_map ? 2 : 1);

			if (*tcount >= Z_MSG_MAX_TABLES || *icount + need > Z_MSG_MAX_ITEMS) {
				obj->type = Z_NONE;
				return Z_FAIL;
			}

			z_obj_table_t *dst = &tables[(*tcount)++];
			dst->len = len;
			dst->a = &items[*icount]; *icount += len;
			dst->b = is_map ? &items[*icount] : NULL;
			if (is_map) *icount += len;

			for (uint32_t i = 0; i < len; i++) {

				z_obj_t *sa = (z_obj_t *)
					z_translate(from_pid, &src->a[i]);
				dst->a[i] = *sa;
				if (z_resolve_obj(from_pid, &dst->a[i],
					tables, tcount, items, icount, blobs, bcount) != Z_OK) {
					obj->type = Z_NONE;
					return Z_FAIL;
				}

				if (is_map) {
					z_obj_t *sb = (z_obj_t *)
						z_translate(from_pid, &src->b[i]);
					dst->b[i] = *sb;
					if (z_resolve_obj(from_pid, &dst->b[i],
						tables, tcount, items, icount, blobs, bcount) != Z_OK) {
						obj->type = Z_NONE;
						return Z_FAIL;
					}
				}

			}

			obj->val.ptr = dst;
			return Z_OK;

		}

		default:
			obj->type = Z_NONE;
			return Z_OK;

	}

}

// -- syscalls --

z_obj_t *k_msg_send(z_obj_t *args) {

	z_msg_t *msg = (z_msg_t *)args;

	if (msg->to >= Z_PROCS_MAX || z_procs[msg->to].base == 0)
		return (&z_fail);

	z_msg_envelope_t env;
	env.to = msg->to;
	env.from = z_pid;	// stamped by the kernel; the caller's `from` is ignored
	env.subject = msg->subject;
	env.tag = msg->tag;

	// scalar payloads are copied as-is here. str/list/map payloads
	// still point into the *sender's* own memory at this point --
	// left untouched; z_msg_read() resolves them, lazily, only if the
	// message actually gets read.
	env.obj = msg->obj;

	if (z_mailbox_push(msg->to, &env) != Z_OK)
		return (&z_fail);

	// The recipient may be blocked waiting for exactly this. Waking it
	// here rather than leaving it to the KTIMER sweep is what makes the
	// latency win real: otherwise a message would sit unnoticed until
	// the next tick, up to ~1.37ms, which is precisely the mushiness
	// this whole change is meant to remove. No-op if it wasn't blocked.
	k_proc_unblock(msg->to);

	return (&z_ok);

}

z_obj_t *k_msg_read(z_obj_t *args) {

	z_msg_t *msg = (z_msg_t *)args;

	z_msg_envelope_t env;
	if (z_mailbox_pop(z_pid, &env) != Z_OK)
		return (&z_fail);

	msg->to = env.to;
	msg->from = env.from;
	msg->subject = env.subject;
	msg->tag = env.tag;
	msg->obj = env.obj;

	uint32_t tcount = 0, icount = 0, bcount = 0;
	z_resolve_obj(env.from, &msg->obj, msg->_tables, &tcount,
		msg->_items, &icount, msg->_blobs, &bcount);
	// on scratch-budget overflow the message is still delivered (so
	// the mailbox doesn't get stuck), but msg->obj comes back as
	// Z_NONE -- see z_resolve_obj().

	return (&z_ok);

}

// -- kernel-side message API for sh.c -- see msg.h for why this
// exists separately from zeitlos.c's app-facing wrappers --

z_rv z_msg_send(z_msg_t *msg) {
	z_obj_t *rv = k_msg_send((z_obj_t *)msg);
	return rv->val.uint32;
}

z_rv z_msg_read(z_msg_t *msg) {
	z_obj_t *rv = k_msg_read((z_obj_t *)msg);
	return rv->val.uint32;
}

z_rv z_msg_wait(z_msg_t *msg, uint32_t subject, uint32_t tag) {
	while (1) {
		if (z_msg_read(msg) == Z_OK) {
			if (msg->subject == subject && msg->tag == tag)
				return Z_OK;
			// not the message we're waiting for -- discard and keep going
		}
	}
}

z_rv z_msg_new_send(uint32_t to, uint32_t subject, uint32_t tag, z_obj_t obj) {
	z_msg_t msg;
	msg.to = to;
	msg.subject = subject;
	msg.tag = tag;
	msg.obj = obj;
	return z_msg_send(&msg);
}

z_rv z_msg_wait_timeout(z_msg_t *msg, uint32_t subject, uint32_t tag, uint32_t timeout_ticks) {
	uint32_t start = z_kernel_ticks;
	while (z_kernel_ticks - start < timeout_ticks) {
		if (z_msg_read(msg) == Z_OK) {
			if (msg->subject == subject && msg->tag == tag)
				return Z_OK;
			// not the message we're waiting for -- discard and keep going
		}
	}
	return Z_FAIL;
}

uint32_t z_uptime_ticks(void) {
	return z_kernel_ticks;
}
