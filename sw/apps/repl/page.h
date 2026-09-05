#ifndef PAGE_H
#define PAGE_H

#include <stdint.h>
#include <stdbool.h>

#include "../../common/zport.h"

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * `page` -- a VT100 text viewer for files of ANY size, in the spirit
 * of less(1). The counterpart to `te` (sw/ext/te, see te_bridge.h and
 * docs/editor.md), which can only EDIT small files: te loads the whole
 * document into repl's heap and pays several times the file's raw size
 * for its line-list representation, which is why te_bridge.c enforces
 * a deliberately conservative TE_MAX_FILE_SIZE ceiling (2KB by
 * default). That ceiling is correct for an editor and useless for
 * reading a book.
 *
 * This module never holds more than a screenful. It keeps the file
 * OPEN across the whole session (fs_open_read()/fs_read_chunk()/
 * fs_seek()/fs_close_handle(), sw/common/zfsapp.h) and re-reads the
 * region it needs on every redraw, so peak memory is a fixed few KB
 * regardless of whether the file is 2KB or 2MB.
 *
 * -- How backward scrolling works, and why it needed a new syscall --
 *
 * Reading forwards is trivial: keep reading. Reading BACKWARDS is the
 * whole design problem, because a byte offset is not a line number and
 * nothing in a text file lets you find the start of the previous line
 * without having already seen it.
 *
 * Holding an offset for every line would solve it and is exactly what
 * this can't do: a 2MB book is ~40k lines, and 40k * 4 bytes is 160KB
 * of index for a process whose entire stack+heap allowance is 64KB
 * (Z_PROC_STACK_SIZE_LARGE, sw/os/kernel.h). So this keeps a SPARSE
 * index instead -- one recorded offset every PAGE_IDX_STRIDE lines,
 * PAGE_IDX_MAX entries, filled in lazily as the reader scrolls. Any
 * jump to line N seeks to the nearest recorded anchor at or before N
 * and scans forward from there, which is at most PAGE_IDX_STRIDE lines
 * of reading -- fast enough to be invisible, and bounded no matter how
 * far into the file the reader has gone.
 *
 * That seek is why Z_SYS_FS_SEEK exists (sw/common/syscalls.def, added
 * with this module). Every pre-existing chunked-I/O call only ever
 * moves forward; without a seek, jumping back one page in a book would
 * mean closing the file, reopening it, and re-reading from byte 0 --
 * on an SD card, for a multi-megabyte file, on every single keypress.
 *
 * Past PAGE_IDX_MAX * PAGE_IDX_STRIDE lines the index stops growing
 * and long backward jumps get slower (they scan from the last anchor).
 * That's a real limit, deliberately chosen over an unbounded index:
 * with the defaults below it's ~65k lines, comfortably past the end of
 * any ordinary book, and the failure mode is "a moment of latency",
 * not "out of memory".
 *
 * -- Session ownership --
 *
 * Exactly the same single-session-per-process rule te_bridge.h
 * documents, for a much simpler reason: the state below is file-static
 * (one open handle, one index, one position), so only one of repl's
 * concurrent `term` connections can be paging at a time. A second
 * `page` command from a different connection while one is live is
 * refused with a clear message rather than quietly stealing the first
 * reader's file position. repl stays fully responsive to its OTHER
 * connections throughout -- paging is driven one keystroke at a time
 * through the normal message loop, and never blocks it.
 */

// how many line-start offsets to remember, and how many lines apart.
// See this header's own comment above for the memory arithmetic --
// PAGE_IDX_MAX * 4 bytes of .bss, covering PAGE_IDX_MAX *
// PAGE_IDX_STRIDE lines before long backward jumps start costing a
// forward scan. 256 * 256 = 65,536 lines for 1KB.
#ifndef PAGE_IDX_MAX
#define PAGE_IDX_MAX 256
#endif
#ifndef PAGE_IDX_STRIDE
#define PAGE_IDX_STRIDE 256
#endif

// starts a paging session for `filename`, with output going to `port`.
//
// on success: returns true, and has ALREADY drawn the first screen --
// the caller should feed every subsequent byte from this connection to
// page_feed() (not the normal line-editing path) until it returns
// false, exactly the contract te_bridge_start() established.
//
// on failure: returns false and writes a short human-readable reason
// into `out` (NUL-terminated, no trailing newline -- dispatch_line()'s
// own convention); nothing was started and page_feed() must not be
// called.
bool page_start(z_port_t *port, const char *filename,
	char *out, uint32_t out_cap);

// feeds one input byte to the live session. Returns true if the
// session is still live (keep feeding), false if the reader just quit
// ('q') -- at which point the caller returns that connection to normal
// line mode. Multi-byte VT100 escape sequences (arrow keys, PgUp/PgDn)
// are reassembled internally across calls, so the caller can keep
// passing bytes through one at a time without knowing about them.
bool page_feed(uint8_t byte);

// true if a session is currently live (process-wide).
bool page_active(void);

// forcibly ends whatever session is live -- for a connection that
// disappeared (Z_PORT_CLOSE) while it owned the pager. Closes the open
// file handle, which matters: handles live in a small, bounded,
// kernel-side table (Z_FS_MAX_OPEN is 8, sw/common/zfs.h) that has no
// process-exit sweep, so leaking one is a genuine, if small, resource
// loss. Safe to call when nothing is active (a no-op).
void page_abort(void);

#endif
