#ifndef XFER_H
#define XFER_H
/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * File transfer over an SSH session: SFTP (sftp.c) and the older scp
 * protocol (scp.c), behind one interface for netserve
 * (docs/netserve.md, "Copying files: sftp and scp").
 *
 * No platform in here, as in the SSH engine: bytes from the client go
 * in (xfer_put()), bytes for the client come out (xfer_out()), and the
 * files are reached through nsfs.h. netserve moves the bytes between
 * this and its ssh_server_t; the host harness (tests/xfer_host.c) does
 * the same over a real socket, for real OpenSSH clients.
 *
 *     sshs_feed --EV_DATA--> xfer_put     xfer_step    xfer_out --> sshs_send
 *                            (XFER_IN)   (files)      (XFER_OUT)
 *
 * The SSH channel's window toward us is XFER_IN: the owner credits the
 * client (sshs_consumed()) with what xfer_step() returns, the bytes it
 * has taken out of the input. Nothing is ever dropped.
 *
 * -- What is kept safe --
 *
 * A file sent here in full is written as NAME.part and renamed to NAME
 * only once it is complete -- the rule every app here follows
 * (docs/philosophy.md, "Trusted"). A transfer that stops part way --
 * the connection drops, the client is stopped, the card fills -- leaves
 * the old NAME as it was, and the .part is removed.
 */
#include <stdint.h>
#include <stdbool.h>

#define XFER_IN         4096    // the client's data, waiting to be used
#define XFER_OUT        2560    // what goes back, waiting for the channel
#define XFER_PATH       256     // Z_FS_PATH_MAX

typedef struct xfer xfer_t;

// A line for the console: what was written, removed, renamed; what failed.
typedef void (*xfer_log_fn)(void *user, const char *line);

// Who may do what: XFER_READONLY refuses everything that would change
// the card -- uploads, removing, renaming, new folders.
#define XFER_READONLY   1u

// SFTP (the subsystem "sftp"). `home`: where relative paths start, and
// what the client is told its directory is.
xfer_t *xfer_sftp(const char *home, uint32_t flags, xfer_log_fn log, void *user);

// scp's own protocol, from the command the client asked to run: "scp -t
// PATH" (sending here) or "scp -f PATH..." (fetching from here). NULL
// with a reason in `why` for a command it does not take.
xfer_t *xfer_scp(const char *cmd, const char *home, uint32_t flags, xfer_log_fn log,
	void *user, char *why, uint32_t why_cap);

// Room for the client's data, and the data.
uint32_t xfer_in_room(const xfer_t *x);
void xfer_put(xfer_t *x, const uint8_t *d, uint32_t n);

// The client sent EOF: it will send no more.
void xfer_eof(xfer_t *x);

// Work on what has arrived, at most `budget` requests -- a few, so the
// other sessions get their turn. Returns the bytes of input used up, to
// credit the channel's window with.
uint32_t xfer_step(xfer_t *x, int budget);

// Bytes for the client, then how many of them went.
const uint8_t *xfer_out(const xfer_t *x, uint32_t *n);
void xfer_sent(xfer_t *x, uint32_t n);

// Over: the protocol has finished and everything is out. `status` is
// the exit status for the client (scp reports 1 if anything failed).
bool xfer_done(const xfer_t *x, uint32_t *status);

// Ends it, whatever its state: files closed, unfinished uploads' .part
// removed, the memory freed.
void xfer_free(xfer_t *x);

// -- shared by sftp.c and scp.c --

// `in` (n bytes, not NUL-terminated) made absolute against `home` and
// tidied: "." and ".." resolved, "//" and a trailing "/" gone. False if
// the result would not fit XFER_PATH less ".part", or `in` holds a NUL
// or a control character.
bool xfer_path(char out[XFER_PATH], const char *home, const char *in, uint32_t n);

// "dir" + "/" + "name", within XFER_PATH less ".part". False if not.
bool xfer_join(char out[XFER_PATH], const char *dir, const char *name);

// The last component of a path ("/" for the root).
const char *xfer_base(const char *path);

// A short English reason for a -Z_E* result.
const char *xfer_strerror(int32_t err);

// The .part rule. xfer_part_open() opens `path`.part for writing
// (removing a stale one first), xfer_part_finish() closes it and puts
// it in place of `path`, xfer_part_abort() closes it and removes it.
// The first returns an fd or -Z_E*; the second 0 or -Z_E*, and on a
// failure after the old file has gone, leaves the .part and sets
// *kept.
int32_t xfer_part_open(const char *path, uint32_t extra_flags);
int32_t xfer_part_finish(int32_t fd, const char *path, bool *kept);
void xfer_part_abort(int32_t fd, const char *path);

// "drwxr-xr-x  1 zeitlos zeitlos  4096 Nov  7 12:51 name" for `ls -l`.
uint32_t xfer_longname(char *out, uint32_t cap, const char *name, uint32_t size,
	bool dir, bool rdonly, uint32_t mtime, uint32_t now);

#endif
