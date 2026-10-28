#ifndef Z_MSG_H
#define Z_MSG_H

#include "kernel.h"

// Of a mailbox's Z_MAILBOX_DEPTH slots, the last this many never take
// input (Z_WM_MOUSE/KEY/WHEEL): they are kept for replies, so a busy
// app's queued keys can never push out the fs reply it is waiting for
// (msg.c, docs/messaging.md, "A full mailbox"). Kernel-private: no app
// depends on it, so it is not part of the flash/card ABI.
#define Z_MAILBOX_RESERVE 8

// -- mailbox primitives -- only the kernel calls these --

z_rv z_mailbox_is_empty(uint32_t pid);
z_rv z_mailbox_is_full(uint32_t pid);
z_rv z_mailbox_push(uint32_t pid, z_msg_envelope_t *msg);
z_rv z_mailbox_pop(uint32_t pid, z_msg_envelope_t *msg);
z_rv z_mailbox_pop_match(uint32_t pid, uint32_t subject, uint32_t tag,
	z_msg_envelope_t *msg);
z_obj_t *k_msg_read_match(z_msg_t *msg, uint32_t subject, uint32_t tag);
// see msg.c -- used by k_proc_wait() to test-and-block atomically
bool z_mailbox_empty(uint32_t pid);
// pushes that found a full mailbox, system-wide -- for sh.c's `ic`
uint32_t k_msg_full_drops(void);
// pointer moves merged into the queued one, and input refused because
// only the reply reserve was left (msg.c, docs/messaging.md)
uint32_t k_msg_coalesced(void);
uint32_t k_msg_input_refused(void);
// process exit (interrupt path): its messages still queued elsewhere
// lose their pointers, and its own mailbox is emptied -- see msg.c
void k_msg_release_pid(uint32_t pid);

// -- syscall handlers, registered in syscalls.def --
//
// named k_msg_* (not z_msg_*) because z_msg_send()/z_msg_read() are
// the app-facing runtime wrappers declared in ../common/zeitlos.h --
// same naming split as k_uart_putc() vs the syscall handler
// z_uart_putc() in uart.c/uart.h.

z_obj_t *k_msg_send(z_obj_t *args);
z_obj_t *k_msg_read(z_obj_t *args);

// -- for sh.c (pid 0, i.e. the kernel itself acting as a process) --
//
// same API shape as z_msg_send/z_msg_read/z_msg_wait/z_msg_new_send
// in ../common/zeitlos.h (the app-facing runtime wrappers), but
// implemented by calling k_msg_send/k_msg_read above directly rather
// than through the z_kernel_ptr syscall trampoline -- sh.c doesn't
// need that indirection, since it IS the kernel, not a separate
// process reaching into it. Deliberately not shared code with
// zeitlos.c: linking zeitlos.o into the kernel build would collide
// with kruntime.c's own getch()/readline()/echo()/noecho(), which
// exist for exactly this reason (kernel-native versions of the same
// functionality apps get via the syscall trampoline).
z_rv z_msg_send(z_msg_t *msg);
z_rv z_msg_read(z_msg_t *msg);
z_rv z_msg_wait(z_msg_t *msg, uint32_t subject, uint32_t tag);
z_rv z_msg_new_send(uint32_t to, uint32_t subject, uint32_t tag, z_obj_t obj);

// same as z_msg_wait(), but gives up (returning Z_FAIL) after
// timeout_ticks with no matching message, instead of waiting forever.
// see sh.c's tget/tput -- z_msg_wait()'s unbounded wait meant an
// unresponsive net process (not running, crashed, stale build with
// no handler for the subject being waited on) hung the shell
// permanently, with no way to recover.
z_rv z_msg_wait_timeout(z_msg_t *msg, uint32_t subject, uint32_t tag, uint32_t timeout_ticks);

// same name/signature as the app-facing z_uptime_ticks() in
// zeitlos.h, so shared code (zstream.c) can call it uniformly
// whether compiled into an app or the kernel -- see z_msg_send()'s
// comment above for the same reasoning applied to messaging.
uint32_t z_uptime_ticks(void);

#endif
