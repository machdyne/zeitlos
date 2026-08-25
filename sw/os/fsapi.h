#ifndef Z_FSAPI_H
#define Z_FSAPI_H

#include <stdint.h>

#include "kernel.h"
#include "../common/zfs.h"

/*
 * Zeitlos OS
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Filesystem syscalls -- Z_SYS_FS_SIZE/_READ/_WRITE, the first way
 * for an ORDINARY APP (as opposed to sh.c, which is kernel code and
 * already links sw/os/fs/fs.c directly) to read or write a file.
 * Added specifically to unblock sw/apps/repl's `te` command (a text
 * editor, see docs/editor.md) but deliberately general -- any future
 * app wanting file access should reach for these rather than each
 * inventing its own.
 *
 * Why a syscall and not a message to some kernel-side service (the
 * TFTP/zstream.h pattern docs/networking.md uses for `net`'s file
 * transfers): those exist because TFTP is fundamentally asynchronous
 * (network I/O, unbounded latency, wants to stream chunk-by-chunk
 * without blocking anything else). A local SD card read/write is
 * neither -- it's the same kind of small, bounded, synchronous
 * operation z_pid_register()/z_pid_lookup() (pidreg.h) already are,
 * so this follows their exact convention instead: a plain syscall,
 * kernel handler runs synchronously in the calling process's own
 * context, done by the time the call returns.
 *
 * Why the caller's own pointers can be dereferenced directly, with no
 * z_translate() (msg.c) needed: z_translate() exists because a
 * MESSAGE's payload was written by some OTHER, not-currently-
 * scheduled process, read later by whichever process happens to call
 * z_msg_read() -- the MTU's 0x8000_0000 mirror is pointing at the
 * READER at that moment, not the original writer, so the pointer has
 * to be re-based by hand. A syscall has no such gap: it's an ordinary
 * (if indirect, via the reg_kernel trampoline) function call, made BY
 * the calling process, and the MTU mirror is still pointing at that
 * SAME process for the syscall's entire duration -- there's no
 * process-switch in between. pidreg.c's own k_pid_register() already
 * relies on exactly this ("no translation needed here, same reasoning
 * as z_ui_print()'s direct obj->val.str") -- this file follows the
 * same precedent, just for filename/buffer pointers instead of a
 * registration name.
 *
 * Why these are plain structs, not z_obj_t/Z_MAP the way most syscall
 * args are: building a Z_MAP from app code means z_obj_map() (zobj.c),
 * which mallocs -- an avoidable allocation for what's otherwise a
 * fixed, small, fully-known argument shape. z_msg_t (zmsg.h) already
 * sets the precedent for a syscall using its OWN dedicated struct
 * instead of z_obj_t's generic shape (k_msg_send()/k_msg_read() cast
 * `args` straight to `z_msg_t *`) -- these three structs do the same
 * thing, sized for exactly what fs_size()/fs_mallocfile()/
 * fs_write_file() (sw/common/zeitlos.c) need.
 *
 * Why no kernel-side malloc(): FatFs itself needs no heap for a
 * single f_open/f_read/f_write/f_close cycle, and this project's own
 * pidreg.c has already found one real newlib-reentrancy landmine in
 * kernel-compiled code (see append_decimal()'s comment there, about
 * snprintf() hanging on real hardware) -- malloc()/free() are exactly
 * the same class of libc-internals-not-set-up-for-kernel-context risk
 * kept far away from this file entirely by not needing them: Z_SYS_
 * FS_READ reads directly into the CALLER's own buffer (already
 * allocated on the app side, e.g. via zeitlos.c's own malloc(), which
 * *is* set up correctly for app-compiled code), same as k_pid_register()
 * writes its result back into caller-owned storage rather than
 * handing back something kernel-allocated.
 *
 * Concurrency note: FatFs's own internal state (this file's `f` local
 * variables aside, sdmm.c/ff.c keep some of their own statics) isn't
 * re-entrant, and unlike z_mailbox_push()/_pop() (msg.c), these
 * handlers do NOT mask interrupts around the underlying f_open/f_read/
 * f_write/f_close calls -- a KTIMER-driven preemption mid-call, onto a
 * different process that ALSO makes an FS_* syscall before the first
 * one's f_close() runs, is a real (if narrow) window for corruption.
 * This exact class of gap already existed before this file for sh.c's
 * own fs_size()/fs_write_file() calls (kernel-native, but sh.c isn't
 * the only thing that can run in kernel context at Z_SYS_UART_* poll
 * points) -- not introduced here, just now reachable from more call
 * sites. Flagged, not fixed, in this revision -- worth real
 * mutual-exclusion (a simple busy flag + maskirq()-protected
 * check-and-set around the whole open/op/close sequence, not just the
 * mailbox-push style single-instruction-ish critical sections msg.c
 * uses) if concurrent FS access from more than one process ever
 * proves to matter in practice. See docs/editor.md for where this is
 * written up alongside `te`'s own memory-budget caveats.
 */

// -- syscall handlers, registered in syscalls.def -- args are
// z_fs_size_args_t/z_fs_read_args_t/z_fs_write_args_t (sw/common/zfs.h),
// cast from the generic z_obj_t* the syscall table's function-pointer
// type requires -- same trick k_msg_send()/k_msg_read() (msg.c) already
// use for z_msg_t*. Same
// mutate-args-in-place-plus-&z_ok/&z_fail-return convention every
// other handler here uses. All three treat "file not found" as a
// normal, non-exceptional outcome (OUT field left at 0, but still
// returns &z_ok, not &z_fail) for FS_SIZE specifically -- matching
// te's own README.md: "opening a file that doesn't exist yet is how
// you create a new one", i.e. callers shouldn't have to distinguish
// "genuinely failed" from "doesn't exist" here. FS_READ/FS_WRITE
// return &z_fail on any real failure (open/read/write error, or a
// requested read bigger than the caller's own `maxlen`).
z_obj_t *k_fs_size(z_obj_t *args);
z_obj_t *k_fs_read(z_obj_t *args);
z_obj_t *k_fs_write(z_obj_t *args);

// added alongside the Files half of the Zeitlos Scheme API
// (docs/scheme_api.md) -- k_fs_unlink() is a thin wrapper over the
// existing fs_unlink() (sw/os/fs/fs.c), which already does the real
// work; k_fs_list() is new (fs_list_dir() only ever PRINTED to the
// console, sh.c-only, never returned anything a caller could use).
// Both follow every convention above: no kernel malloc, caller-owned
// buffers, mutate-args-in-place-plus-&z_ok/&z_fail-return.
z_obj_t *k_fs_unlink(z_obj_t *args);
z_obj_t *k_fs_list(z_obj_t *args);

// chunked file I/O -- see zfs.h's own comment for the full design
// writeup (why a kernel-side handle table, ownership-by-pid, the
// known limitation on a crashed process's handles).
// mkdir/touch: previously sh.c-only (it links fs/fs.c directly), now
// reachable from any app -- added for the Scheme API's (mkdir ...) and
// (touch-file ...), see docs/scheme_api.md. Both are thin wrappers over
// the existing fs_mkdir()/fs_touch(), same relationship k_fs_unlink()
// has to fs_unlink(). Args are z_fs_path_args_t (sw/common/zfs.h).
z_obj_t *k_fs_mkdir(z_obj_t *args);
z_obj_t *k_fs_touch(z_obj_t *args);

// repositions an open chunked handle -- args are z_fs_seek_args_t.
// Added for sw/apps/repl's `page`, whose backward scrolling needs to
// re-read regions already behind the current position; every other
// chunked call only ever moves forward. See k_fs_seek()'s own comment
// in fsapi.c for the EOF-clamping behavior.
z_obj_t *k_fs_seek(z_obj_t *args);

// filesystem capacity -- args are z_fs_df_args_t (sw/common/zfs.h),
// both figures in KB. Thin wrapper over the long-existing but
// previously unreachable fs_total()/fs_free().
z_obj_t *k_fs_df(z_obj_t *args);

z_obj_t *k_fs_open_write(z_obj_t *args);
z_obj_t *k_fs_open_read(z_obj_t *args);
z_obj_t *k_fs_read_chunk(z_obj_t *args);
z_obj_t *k_fs_write_chunk(z_obj_t *args);
z_obj_t *k_fs_close(z_obj_t *args);

#endif
