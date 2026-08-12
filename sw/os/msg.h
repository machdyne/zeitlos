#ifndef Z_MSG_H
#define Z_MSG_H

#include "kernel.h"

// -- mailbox primitives -- only the kernel calls these --

z_rv z_mailbox_is_empty(uint32_t pid);
z_rv z_mailbox_is_full(uint32_t pid);
z_rv z_mailbox_push(uint32_t pid, z_msg_envelope_t *msg);
z_rv z_mailbox_pop(uint32_t pid, z_msg_envelope_t *msg);

// -- syscall handlers, registered in syscalls.def --
//
// named k_msg_* (not z_msg_*) because z_msg_send()/z_msg_read() are
// the app-facing runtime wrappers declared in ../common/zeitlos.h --
// same naming split as k_uart_putc() vs the syscall handler
// z_uart_putc() in uart.c/uart.h.

z_obj_t *k_msg_send(z_obj_t *args);
z_obj_t *k_msg_read(z_obj_t *args);

#endif
