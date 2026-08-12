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
static inline void *z_translate(uint32_t pid, void *vptr) {
	if (!vptr) return NULL;
	return (void *)((uint32_t)vptr - 0x80000000 + z_procs[pid].base);
}

// resolve a z_obj_t that (possibly) belongs to process `from_pid` so
// it's safe for the *current* process to read. scalars are untouched.
// strings are pointed at the sender's bytes via a physical address --
// no copy. lists/maps have their structural nodes rebuilt into the
// caller-supplied scratch arrays (see zmsg.h); the leaf data inside
// them is left in place in the sender's memory. never mutates the
// sender's own memory.
//
// returns Z_FAIL (and resets obj to Z_NONE) if the scratch budget
// isn't big enough for the payload.
static z_rv z_resolve_obj(uint32_t from_pid, z_obj_t *obj,
	z_obj_table_t *tables, uint32_t *tcount,
	z_obj_t *items, uint32_t *icount) {

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
					tables, tcount, items, icount) != Z_OK) {
					obj->type = Z_NONE;
					return Z_FAIL;
				}

				if (is_map) {
					z_obj_t *sb = (z_obj_t *)
						z_translate(from_pid, &src->b[i]);
					dst->b[i] = *sb;
					if (z_resolve_obj(from_pid, &dst->b[i],
						tables, tcount, items, icount) != Z_OK) {
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

	uint32_t tcount = 0, icount = 0;
	z_resolve_obj(env.from, &msg->obj, msg->_tables, &tcount, msg->_items, &icount);
	// on scratch-budget overflow the message is still delivered (so
	// the mailbox doesn't get stuck), but msg->obj comes back as
	// Z_NONE -- see z_resolve_obj().

	return (&z_ok);

}
