/*
 * Zeitlos OS
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * PID name registry. See pidreg.h.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "kernel.h"
#include "pidreg.h"

typedef struct {
	char		name[Z_PIDREG_NAME_MAX];	// full name, e.g. "term3"
	uint32_t	pid;
	bool		active;
} z_pidreg_entry_t;

static __attribute__((section(".bss"))) z_pidreg_entry_t z_pidreg[Z_PIDREG_MAX];

// appends val's decimal digits to the end of the NUL-terminated
// string already in buf (bufsize is buf's total capacity, including
// the NUL). Deliberately NOT snprintf(): snprintf/vsnprintf had never
// been called from kernel-compiled code before this file (confirmed
// against this project's history -- every prior use was in app code)
// and hung on its very first call here on real hardware, almost
// certainly a newlib-style reentrancy-lock/stdio-internals issue that
// works fine for apps but isn't set up the same way for the kernel's
// own build/link -- see the standalone debugging session that found
// this. Matches this file's neighbors' own precedent (kprint(),
// kprint_hex32() in kernel.c) for exactly this reason: don't lean on
// full libc formatting machinery from kernel-compiled code, write the
// small amount actually needed by hand instead.
static void append_decimal(char *buf, uint32_t bufsize, uint32_t val) {
	uint32_t len = strlen(buf);
	if (len >= bufsize - 1) return;	// no room at all

	char digits[10];	// max uint32_t is 10 decimal digits, no NUL needed here
	int di = 0;
	if (val == 0) {
		digits[di++] = '0';
	} else {
		while (val > 0 && di < 10) {
			digits[di++] = '0' + (val % 10);
			val /= 10;
		}
	}

	// digits[] was filled least-significant-first -- write it out
	// reversed, respecting bufsize
	while (di > 0 && len < bufsize - 1)
		buf[len++] = digits[--di];
	buf[len] = 0;
}

// see pidreg.h -- must run before anything can reach k_pid_register()/
// k_pid_lookup(), since .bss isn't reliably zero at boot here.
void k_pidreg_init(void) {
	for (int e = 0; e < Z_PIDREG_MAX; e++) {
		z_pidreg[e].active = false;
		z_pidreg[e].pid = 0;
		z_pidreg[e].name[0] = 0;
	}
}

// -- syscalls --

// Name -> pid, callable from anywhere in the kernel including
// INTERRUPT CONTEXT.
//
// Separate from k_pid_lookup() below, which is the syscall: that one
// takes and returns z_obj_t and allocates, neither of which is safe
// from an ISR. This is a table scan and nothing else.
//
// Added for the ethernet receive interrupt (k_irq_handler() in
// kernel.c), which has to find whoever is waiting on packets without
// hardwiring a pid -- net registers itself like any other service and
// a fixed pid would break the moment it were restarted.
bool k_pid_find(const char *name, uint32_t *pid) {

	if (!name || !pid) return false;

	for (int e = 0; e < Z_PIDREG_MAX; e++) {
		if (!z_pidreg[e].active) continue;
		if (strcmp(z_pidreg[e].name, name) != 0) continue;
		*pid = z_pidreg[e].pid;
		return true;
	}

	return false;

}

z_obj_t *k_pid_register(z_obj_t *args) {

	if (args->type != Z_STR || !args->val.str) {
		args->type = Z_NONE;
		return &z_fail;
	}

	// copy+bound the caller-supplied base name defensively -- a
	// syscall argument comes straight from app code (no translation
	// needed here, same reasoning as z_ui_print()'s direct obj->val.str
	// dereference in ui.c: a syscall runs synchronously in the
	// calling process's own already-active MTU mapping, unlike a
	// message payload read later by a DIFFERENT scheduled process,
	// which is what msg.c's z_translate() exists for), but don't
	// trust it to be NUL-terminated within any particular length.
	char basename[Z_PIDREG_BASENAME_MAX];
	uint32_t i;
	for (i = 0; i < Z_PIDREG_BASENAME_MAX - 1 && args->val.str[i]; i++)
		basename[i] = args->val.str[i];
	basename[i] = 0;

	if (i == 0) {
		args->type = Z_NONE;
		return &z_fail;	// empty base name
	}

	// find the smallest N >= 0 such that "basename"+N isn't already
	// an ACTIVE full name in the table. bounded: this can only loop
	// once per already-active entry sharing this exact basename+N
	// pattern, and there are at most Z_PIDREG_MAX entries total, so
	// it always terminates well within that many iterations.
	char candidate[Z_PIDREG_NAME_MAX];
	uint32_t n = 0;
	for (;;) {
		strncpy(candidate, basename, Z_PIDREG_NAME_MAX - 1);
		candidate[Z_PIDREG_NAME_MAX - 1] = 0;
		append_decimal(candidate, Z_PIDREG_NAME_MAX, n);
		bool taken = false;
		for (int e = 0; e < Z_PIDREG_MAX; e++) {
			if (z_pidreg[e].active && strcmp(z_pidreg[e].name, candidate) == 0) {
				taken = true;
				break;
			}
		}
		if (!taken) break;
		n++;
	}

	// find a free slot to actually store it in -- separate from the
	// name-collision search above: the candidate name might be free
	// while every physical slot is still full of OTHER base names'
	// entries.
	int slot = -1;
	for (int e = 0; e < Z_PIDREG_MAX; e++) {
		if (!z_pidreg[e].active) { slot = e; break; }
	}
	if (slot < 0) {
		args->type = Z_NONE;
		return &z_fail;	// registry full
	}

	strncpy(z_pidreg[slot].name, candidate, Z_PIDREG_NAME_MAX - 1);
	z_pidreg[slot].name[Z_PIDREG_NAME_MAX - 1] = 0;
	z_pidreg[slot].pid = z_pid;	// stamped by the kernel -- the caller
									// can't register on behalf of a
									// different pid, same as
									// k_msg_send()'s `from`
	z_pidreg[slot].active = true;

	// mutate args in place for the output, same convention every
	// other syscall handler in this codebase uses (z_uptime(),
	// z_hid_read_key(), k_msg_read()) -- NOT returning a fresh
	// data-carrying object, since &z_ok/&z_fail are `static` (one
	// private copy per translation unit -- see zobj.h), so a pointer
	// returned from a kernel-compiled handler is never == an app's
	// own &z_ok. Callers on both sides of the syscall boundary check
	// success by the VALUE at the returned pointer (rv->val.uint32 ==
	// Z_OK), not by pointer identity -- same as z_msg_send() already
	// does.
	args->type = Z_STR;
	args->val.str = z_pidreg[slot].name;
	return &z_ok;

}

z_obj_t *k_pid_lookup(z_obj_t *args) {

	if (args->type != Z_STR || !args->val.str) {
		args->type = Z_NONE;
		return &z_fail;
	}

	char name[Z_PIDREG_NAME_MAX];
	uint32_t i;
	for (i = 0; i < Z_PIDREG_NAME_MAX - 1 && args->val.str[i]; i++)
		name[i] = args->val.str[i];
	name[i] = 0;

	for (int e = 0; e < Z_PIDREG_MAX; e++) {
		if (z_pidreg[e].active && strcmp(z_pidreg[e].name, name) == 0) {
			args->type = Z_UINT32;
			args->val.uint32 = z_pidreg[e].pid;
			return &z_ok;
		}
	}

	args->type = Z_NONE;
	return &z_fail;

}

const char *k_pidreg_name_for(uint32_t pid) {

	for (uint32_t i = 0; i < Z_PIDREG_MAX; i++)
		if (z_pidreg[i].active && z_pidreg[i].pid == pid)
			return z_pidreg[i].name;

	return NULL;

}

void k_pidreg_release_all(uint32_t pid) {
	for (int e = 0; e < Z_PIDREG_MAX; e++) {
		if (z_pidreg[e].active && z_pidreg[e].pid == pid) {
			z_pidreg[e].active = false;
			z_pidreg[e].name[0] = 0;
			z_pidreg[e].pid = 0;
		}
	}
}

// see pidreg.h -- plain printf() here, not the append_decimal()
// workaround above: that workaround is specifically about avoiding
// snprintf()/vsnprintf() (formatting INTO a buffer), not printf()
// itself (formatting straight to stdout/UART) -- printf() is already
// used throughout kernel.c (including k_proc_dump(), "ps", this
// function's direct counterpart) with no issue, on the same hardware
// this bug was found on.
z_rv k_pidreg_dump(void) {
	int shown = 0;
	for (int e = 0; e < Z_PIDREG_MAX; e++) {
		if (!z_pidreg[e].active) continue;
		printf(" slot: %2i name: %-15s pid: %lu\n",
			e, z_pidreg[e].name, z_pidreg[e].pid);
		shown++;
	}
	if (!shown) printf(" (empty)\n");
	return Z_OK;
}

// -- kernel-native wrappers for sh.c -- see pidreg.h for why these
// exist separately from ../common/zeitlos.c's app-facing versions.
// same in-place-mutation convention as the syscall handlers above
// (unsurprising, since these just call them directly) -- build a
// LOCAL z_obj_t, call the handler, read the value back out of that
// same local object rather than trusting the returned pointer's
// identity (see k_pid_register()'s comment above for why).

bool z_pid_register(const char *basename, char *out, uint32_t outlen) {

	if (outlen == 0)
		return false;

	z_obj_t obj;
	obj.type = Z_STR;
	obj.val.str = (char *)basename;

	z_obj_t *rv = k_pid_register(&obj);
	if (rv->val.uint32 != Z_OK || obj.type != Z_STR || !obj.val.str)
		return false;

	uint32_t i;
	for (i = 0; i < outlen - 1 && obj.val.str[i]; i++)
		out[i] = obj.val.str[i];
	out[i] = 0;

	return true;

}

bool z_pid_lookup(const char *name, uint32_t *pid) {

	z_obj_t obj;
	obj.type = Z_STR;
	obj.val.str = (char *)name;

	z_obj_t *rv = k_pid_lookup(&obj);
	if (rv->val.uint32 != Z_OK || obj.type != Z_UINT32)
		return false;

	*pid = obj.val.uint32;
	return true;

}
