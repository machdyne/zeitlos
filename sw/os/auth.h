#ifndef AUTH_H
#define AUTH_H
/*
 * Zeitlos OS
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The password and the lock policy, the kernel's side: Z_SYS_AUTH,
 * and the console's passwd and lock. docs/security.md.
 */
#include <stdint.h>
#include <stdbool.h>
#include "../common/zobj.h"
#include "../common/zauth.h"

// One boot-log line: whether there is a password, and the policy.
void k_auth_init(void);

z_obj_t *k_auth(z_obj_t *args);                 // Z_SYS_AUTH

// The one-check-at-a-time gate, if a process died holding it.
// Interrupt path: forgets, nothing more.
void k_auth_release_pid(uint32_t pid);

void k_auth_status(z_auth_status_t *st);

// The console: `passwd`, `passwd reset`, `lock`. `line_from_uart` says
// whether the command line itself was typed on UART0 rather than
// injected through console0 -- `passwd reset` requires it.
void k_auth_shell_passwd(const char *sub, bool line_from_uart);
void k_auth_shell_lock(void);

// Before the console's first prompt: asks for the password if the
// console lock is on (sys.lock.console) and a password is set.
void k_auth_console_boot(void);

#endif
