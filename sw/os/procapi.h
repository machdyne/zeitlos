#ifndef Z_PROCAPI_H
#define Z_PROCAPI_H

#include "kernel.h"
#include "../common/zproc.h"

/*
 * Zeitlos OS
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Process-table syscall handlers -- see procapi.c for the writeup.
 */

// Z_SYS_PROC_LIST -- args are z_proc_list_args_t (sw/common/zproc.h).
// Fills the caller's array with one entry per live process slot.
// &z_fail only for a genuinely unusable request (NULL/zero-capacity
// output); a table with nothing in it is a normal, successful listing
// of zero entries, not a failure.
z_obj_t *k_proc_list(z_obj_t *args);

#endif
