/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * scp's own protocol, the one from rcp: what `scp -O`, older scp
 * programs and PuTTY's pscp (as a fallback) speak. The client runs
 * "scp -t PATH" here to send files, "scp -f PATH..." to fetch them, and
 * the two sides exchange one-line headers, file data and one-byte
 * acknowledgements. Modern scp uses SFTP instead (sftp.c), so this is
 * the older path -- kept because it is what a great many machines
 * still have. docs/netserve.md, "Copying files: sftp and scp".
 *
 *   C0644 SIZE NAME\n   a file follows, SIZE bytes, then a 0 byte
 *   D0755 0 NAME\n      a folder: what follows goes in it ...
 *   E\n                 ... until this
 *   T MTIME 0 ATIME 0\n the times of the next file or folder (-p)
 *   \0                  yes / ok
 *   \1 text\n           an error: that one failed, carry on
 *   \2 text\n           an error: stop
 *
 * No shell runs the command, so its words are split here, with quotes
 * and backslashes as a shell would; a source path may end in a pattern
 * with * and ? (matched as FAT matches names, ignoring case).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xfer_int.h"

#define LINE_MAX_   (XFER_PATH + 64)
#define ARGS_MAX    1024
#define DEPTH_MAX   16

enum { SINK = 1, SOURCE };

// sink states
enum { K_START, K_HEADER, K_DATA, K_DATA_END };
// source states
enum { R_WAIT_GO, R_NEXT, R_ACK, R_DATA };
// what a source does once the client acknowledges
enum { A_T, A_C, A_DATA_END, A_D, A_E };

typedef struct {
	uint32_t idx;                   // entries of this folder already sent
	uint16_t plen;                  // `cur` cut here is this folder
	bool glob;                      // the top-level pattern's folder: no D/E
} frame_t;

typedef struct {
	xfer_t x;
	int mode;
	bool recurse, times, want_dir;
	uint32_t errors;

	char line[LINE_MAX_];           // a header or a message, as it arrives
	uint32_t line_len;

	// sink
	int ks;
	char target[XFER_PATH];
	bool target_dir;
	int depth;                      // folders entered below the target
	char cur[XFER_PATH];            // the folder being written into (depth > 0)
	int32_t fd;
	char fpath[XFER_PATH];
	uint32_t left;
	int32_t werr;

	// source
	int rs, after;
	char args[ARGS_MAX];            // the paths, NUL-separated
	uint32_t args_len, arg_at;
	frame_t fr[DEPTH_MAX];
	int nfr;
	char pat[XFER_PATH];            // a top-level pattern, matched in fr[0]
	char item[XFER_PATH];           // the file or folder being sent
	z_fs_info_t info;
	uint32_t sent;
} scp_t;

// -- small helpers --

static bool emit_str(scp_t *s, const char *t) { return xfer_emit(&s->x, t, (uint32_t)strlen(t)); }

// "\1scp: what\n" to the client, and the console.
static void error1(scp_t *s, const char *what) {
	char m[LINE_MAX_ + 16];
	snprintf(m, sizeof(m), "\1scp: %s\n", what);
	xfer_emit(&s->x, m, (uint32_t)strlen(m));
	xfer_logf(&s->x, "scp: %s", what);
	s->errors++;
}

static void fatal(scp_t *s, const char *what) {
	char m[LINE_MAX_ + 16];
	snprintf(m, sizeof(m), "\2scp: %s\n", what);
	xfer_emit(&s->x, m, (uint32_t)strlen(m));
	xfer_logf(&s->x, "scp: %s", what);
	s->errors++;
	s->x.done = true;
	s->x.status = 1;
}

// A whole line from the input into s->line (without the \n). Returns
// true when there is one; the bytes used are added to *used.
static bool get_line(scp_t *s, uint32_t *used) {
	while (s->x.in_len) {
		uint8_t c;
		xfer_in_peek(&s->x, &c, 1);
		xfer_in_take(&s->x, 1);
		(*used)++;
		if (c == '\n') { s->line[s->line_len] = 0; return true; }
		if (s->line_len < sizeof(s->line) - 1) s->line[s->line_len++] = (char)c;
	}
	return false;
}

static bool name_ok(const char *n) {
	return *n && strcmp(n, ".") && strcmp(n, "..") && !strchr(n, '/');
}

// -- the sink: files sent here (scp -t) --

static bool sink_place(scp_t *s, const char *name, char out[XFER_PATH]) {
	if (!name_ok(name)) return false;
	if (s->depth) return xfer_join(out, s->cur, name);
	if (s->target_dir) return xfer_join(out, s->target, name);
	snprintf(out, XFER_PATH, "%s", s->target);
	return true;
}

static void sink_header(scp_t *s) {
	char path[XFER_PATH], m[XFER_PATH + 64];
	char *l = s->line;

	if (l[0] == '\1' || l[0] == '\2') {		// the client's own error
		xfer_logf(&s->x, "scp: the client says: %s", l + 1);
		s->errors++;
		if (l[0] == '\2') { s->x.done = true; s->x.status = 1; }
		return;
	}
	if (l[0] == 'T') { xfer_emit(&s->x, "", 1); return; }	// times: FAT keeps its own
	if (l[0] == 'E') {
		if (!s->depth) { fatal(s, "protocol error: E outside a folder"); return; }
		char *slash = strrchr(s->cur, '/');
		if (--s->depth && slash) *slash = 0;
		xfer_emit(&s->x, "", 1);
		return;
	}
	if (l[0] != 'C' && l[0] != 'D') { fatal(s, "protocol error: unexpected header"); return; }

	// "C0644 1234 name" / "D0755 0 name"
	char *p = l + 1;
	for (int i = 0; i < 4; i++) if (*p >= '0' && *p <= '7') p++; else { fatal(s, "protocol error: bad mode"); return; }
	if (*p++ != ' ') { fatal(s, "protocol error: bad header"); return; }
	uint64_t size = 0;
	if (*p < '0' || *p > '9') { fatal(s, "protocol error: bad size"); return; }
	while (*p >= '0' && *p <= '9') { size = size * 10 + (uint64_t)(*p - '0'); if (size > 0xFFFFFFFFull) break; p++; }
	if (*p++ != ' ') { fatal(s, "protocol error: bad size"); return; }
	const char *name = p;

	if (!sink_place(s, name, path)) { fatal(s, "protocol error: bad name"); return; }
	z_fs_info_t fi;
	bool exists = nsfs_stat(path, &fi) >= 0;

	if (l[0] == 'D') {
		if (!s->recurse) { fatal(s, "received a folder without -r"); return; }
		if (s->x.flags & XFER_READONLY) { fatal(s, "read-only: apps.netserve.sftp is \"read\""); return; }
		if (s->depth >= DEPTH_MAX) { fatal(s, "folders nested too deep"); return; }
		if (exists && fi.type != Z_FS_TYPE_DIR) {
			snprintf(m, sizeof(m), "%s: Not a directory", path);
			fatal(s, m);
			return;
		}
		if (!exists) {
			int32_t rc = nsfs_mkdir(path);
			if (rc < 0) { snprintf(m, sizeof(m), "%s: %s", path, xfer_strerror(rc)); fatal(s, m); return; }
			xfer_logf(&s->x, "scp: made %s/", path);
		}
		snprintf(s->cur, sizeof(s->cur), "%s", path);
		s->depth++;
		xfer_emit(&s->x, "", 1);
		return;
	}

	// a file: refused before its data, or taken
	if (s->x.flags & XFER_READONLY) { error1(s, "read-only: apps.netserve.sftp is \"read\""); return; }
	if (size > 0xFFFFFFFFull) { snprintf(m, sizeof(m), "%s: File too large", path); error1(s, m); return; }
	if (exists && fi.type == Z_FS_TYPE_DIR) { snprintf(m, sizeof(m), "%s: Is a directory", path); error1(s, m); return; }
	int32_t fd = xfer_part_open(path, 0);
	if (fd < 0) { snprintf(m, sizeof(m), "%s: %s", path, xfer_strerror(fd)); error1(s, m); return; }
	s->fd = fd;
	snprintf(s->fpath, sizeof(s->fpath), "%s", path);
	s->left = (uint32_t)size;
	s->werr = 0;
	xfer_emit(&s->x, "", 1);
	s->ks = K_DATA;
}

static void sink_file_end(scp_t *s, uint8_t client_status) {
	char m[XFER_PATH + 96];
	uint32_t size_was = 0;	// for the console
	z_fs_info_t fi;
	if (nsfs_fstat(s->fd, &fi) >= 0) size_was = fi.size;
	if (client_status || s->werr < 0) {
		xfer_part_abort(s->fd, s->fpath);
		if (s->werr < 0) {
			snprintf(m, sizeof(m), "%s: %s -- the old file is as it was", s->fpath, xfer_strerror(s->werr));
			error1(s, m);
		} else {
			xfer_logf(&s->x, "scp: %s not received in full -- the old file is as it was", s->fpath);
			s->errors++;
		}
		return;
	}
	bool kept;
	int32_t rc = xfer_part_finish(s->fd, s->fpath, &kept);
	if (rc < 0) {
		if (kept) snprintf(m, sizeof(m), "received, but kept as %.250s.part: %s", s->fpath, xfer_strerror(rc));
		else snprintf(m, sizeof(m), "%s: %s -- the old file is as it was", s->fpath, xfer_strerror(rc));
		error1(s, m);
		return;
	}
	xfer_logf(&s->x, "scp: received %s (%lu bytes)", s->fpath, (unsigned long)size_was);
	xfer_emit(&s->x, "", 1);
}

static uint32_t sink_step(scp_t *s, int budget) {
	uint32_t used = 0;
	while (budget > 0 && !s->x.done) {
		if (xfer_out_room(&s->x) < LINE_MAX_ + 32) break;
		switch (s->ks) {
		case K_START:
			xfer_emit(&s->x, "", 1);            // ready
			s->ks = K_HEADER;
			break;
		case K_HEADER:
			if (!get_line(s, &used)) return used;
			s->line_len = 0;
			sink_header(s);
			budget--;
			break;
		case K_DATA: {
			uint32_t n;
			const uint8_t *p = xfer_in_front(&s->x, &n);
			if (n > s->left) n = s->left;
			if (s->left && !n) return used;
			if (n && !s->werr) {
				int32_t rc = nsfs_write(s->fd, p, n);
				if (rc < 0) s->werr = rc;
				else if ((uint32_t)rc < n) s->werr = -Z_ENOSPC;
			}
			xfer_in_take(&s->x, n);
			used += n;
			s->left -= n;
			if (!s->left) s->ks = K_DATA_END;
			break;
		}
		case K_DATA_END: {
			uint8_t c;
			if (!s->x.in_len) return used;
			xfer_in_peek(&s->x, &c, 1);
			if (c) {
				// \1 or \2 and a message: the client failed to read it
				if (!get_line(s, &used)) return used;
				s->line_len = 0;
				xfer_logf(&s->x, "scp: the client says: %s", s->line + 1);
			} else {
				xfer_in_take(&s->x, 1);
				used++;
			}
			sink_file_end(s, c);
			s->ks = K_HEADER;
			budget--;
			break;
		}
		}
	}
	return used;
}

// -- the source: files fetched from here (scp -f) --

// Glob, as FAT compares names: * and ?, ignoring ASCII case.
static bool match(const char *p, const char *n) {
	for (; *p; p++, n++) {
		if (*p == '*') {
			while (p[1] == '*') p++;
			if (!p[1]) return true;
			for (; *n; n++) if (match(p + 1, n)) return true;
			return false;
		}
		if (!*n) return false;
		char a = *p, b = *n;
		if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
		if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
		if (*p != '?' && a != b) return false;
	}
	return !*n;
}

static bool has_glob(const char *t) { return strpbrk(t, "*?") != NULL; }

// The folder fr[k] is: `cur` up to plen.
static void frame_dir(scp_t *s, int k, char out[XFER_PATH]) {
	memcpy(out, s->cur, s->fr[k].plen);
	out[s->fr[k].plen] = 0;
	if (!out[0]) { out[0] = '/'; out[1] = 0; }
}

// The next entry of the innermost folder, into s->item. False at its
// end. Folders are read again from the start each time, the entries
// already sent skipped: the fs driver has four folder slots for the
// whole machine, so one cannot be held open per level.
static bool frame_next(scp_t *s) {
	frame_t *f = &s->fr[s->nfr - 1];
	char dir[XFER_PATH], name[XFER_PATH];
	z_fs_info_t fi;
	nsfs_dir_t *d;
	uint32_t i = 0;
	bool found = false;

	frame_dir(s, s->nfr - 1, dir);
	if (nsfs_opendir(dir, &d) < 0) return false;
	while (nsfs_readdir(d, name, sizeof(name), &fi) > 0) {
		if (f->glob && !match(s->pat, name)) continue;
		if (i++ < f->idx) continue;
		if (!xfer_join(s->item, dir, name)) {
			char m[XFER_PATH + 32];
			snprintf(m, sizeof(m), "%.120s/%.120s: Name too long", dir, name);
			error1(s, m);
			f->idx++;               // = i: skipped from now on
			continue;
		}
		s->info = fi;
		found = true;
		break;
	}
	nsfs_closedir(d);
	if (found) f->idx++;
	return found;
}

static void src_wait(scp_t *s, int after) {
	s->after = after;
	s->rs = R_ACK;
}

static void src_T(scp_t *s) {
	char t[48];
	uint32_t m = z_fs_info_time(&s->info);
	snprintf(t, sizeof(t), "T%lu 0 %lu 0\n", (unsigned long)m, (unsigned long)m);
	emit_str(s, t);
}

// The header of s->item, a file's (C) or a folder's (D).
static void src_header(scp_t *s) {
	char h[XFER_PATH + 32];
	const char *name = xfer_base(s->item);
	if (s->info.type == Z_FS_TYPE_DIR) {
		snprintf(h, sizeof(h), "D0755 0 %.255s\n", name);
		emit_str(s, h);
		src_wait(s, A_D);
		return;
	}
	snprintf(h, sizeof(h), "C%04o %lu %.255s\n", (s->info.attr & Z_FS_ATTR_RDONLY) ? 0444 : 0644,
		(unsigned long)s->info.size, name);
	emit_str(s, h);
	src_wait(s, A_C);
}

// Sends s->item (its info in s->info): its times first if -p, each line
// acknowledged before the next.
static void src_item(scp_t *s) {
	char h[XFER_PATH + 32];
	if (s->info.type == Z_FS_TYPE_DIR) {
		if (!s->recurse) {
			snprintf(h, sizeof(h), "%.255s: not a regular file", s->item);
			error1(s, h);
			return;
		}
		if (s->nfr >= DEPTH_MAX) { snprintf(h, sizeof(h), "%.255s: folders nested too deep", s->item); error1(s, h); return; }
	} else {
		int32_t fd = nsfs_open(s->item, Z_O_RDONLY, NULL);
		if (fd < 0) {
			snprintf(h, sizeof(h), "%.255s: %s", s->item, xfer_strerror(fd));
			error1(s, h);
			return;
		}
		s->fd = fd;
		s->sent = 0;
	}
	if (s->times) { src_T(s); src_wait(s, A_T); return; }
	src_header(s);
}

// The next top-level path: a file or folder, or a pattern's folder.
static bool src_next_arg(scp_t *s) {
	char path[XFER_PATH], m[XFER_PATH + 32];
	while (s->arg_at < s->args_len) {
		const char *a = s->args + s->arg_at;
		s->arg_at += (uint32_t)strlen(a) + 1;
		if (!xfer_path(path, s->x.home, a, (uint32_t)strlen(a))) {
			snprintf(m, sizeof(m), "%.200s: Name too long", a);
			error1(s, m);
			continue;
		}
		const char *base = xfer_base(path);
		if (has_glob(base)) {
			snprintf(s->pat, sizeof(s->pat), "%s", base);
			snprintf(s->cur, sizeof(s->cur), "%s", path);
			s->cur[base - path > 1 ? base - path - 1 : 0] = 0;
			s->fr[0].plen = (uint16_t)strlen(s->cur);
			s->fr[0].idx = 0;
			s->fr[0].glob = true;
			s->nfr = 1;
			return true;
		}
		int32_t rc = nsfs_stat(path, &s->info);
		if (rc < 0) {
			snprintf(m, sizeof(m), "%s: %s", path, xfer_strerror(rc));
			error1(s, m);
			continue;
		}
		snprintf(s->item, sizeof(s->item), "%s", path);
		src_item(s);
		return true;
	}
	return false;
}

static void src_next(scp_t *s) {
	if (s->nfr) {
		if (frame_next(s)) { src_item(s); return; }
		// this folder is done
		bool glob = s->fr[s->nfr - 1].glob;
		s->nfr--;
		if (glob) {
			if (!s->fr[0].idx) {
				char m[XFER_PATH + 32];
				snprintf(m, sizeof(m), "%s: No match", s->pat);
				error1(s, m);
			}
			return;
		}
		emit_str(s, "E\n");
		src_wait(s, A_E);
		return;
	}
	if (!src_next_arg(s)) {
		s->x.done = true;
		s->x.status = s->errors ? 1 : 0;
	}
}

static void src_acked(scp_t *s, bool ok) {
	s->rs = R_NEXT;
	switch (s->after) {
	case A_T:
		if (ok) { src_header(s); return; }
		if (s->info.type != Z_FS_TYPE_DIR) nsfs_close(s->fd);
		return;
	case A_C:
		if (!ok) { nsfs_close(s->fd); return; }	// the client will not take it
		s->rs = R_DATA;
		return;
	case A_DATA_END:
		if (ok) xfer_logf(&s->x, "scp: sent %s (%lu bytes)", s->item, (unsigned long)s->sent);
		return;
	case A_D:
		if (!ok) return;
		// enter it: cur becomes the folder
		snprintf(s->cur, sizeof(s->cur), "%s", s->item);
		s->fr[s->nfr].plen = (uint16_t)strlen(s->cur);
		s->fr[s->nfr].idx = 0;
		s->fr[s->nfr].glob = false;
		s->nfr++;
		return;
	case A_E:
		return;
	}
}

static uint32_t source_step(scp_t *s, int budget) {
	uint32_t used = 0;
	while (budget > 0 && !s->x.done) {
		if (xfer_out_room(&s->x) < LINE_MAX_ + 32) break;
		switch (s->rs) {
		case R_WAIT_GO:
		case R_ACK: {
			uint8_t c;
			if (!s->x.in_len) {
				if (s->x.eof) { s->x.done = true; s->x.status = 1; }
				return used;
			}
			xfer_in_peek(&s->x, &c, 1);
			if (c) {
				if (!get_line(s, &used)) return used;
				s->line_len = 0;
				xfer_logf(&s->x, "scp: the client says: %s", s->line + 1);
				s->errors++;
				if (c == 2 || s->rs == R_WAIT_GO) { s->x.done = true; s->x.status = 1; return used; }
				src_acked(s, false);
			} else {
				xfer_in_take(&s->x, 1);
				used++;
				if (s->rs == R_WAIT_GO) s->rs = R_NEXT;
				else src_acked(s, true);
			}
			budget--;
			break;
		}
		case R_NEXT:
			src_next(s);
			budget--;
			break;
		case R_DATA: {
			uint32_t want = s->info.size - s->sent, room = xfer_out_room(&s->x);
			if (want > room) want = room;
			if (want > 2048) want = 2048;
			if (want) {
				int32_t rc = nsfs_read(s->fd, s->x.out + s->x.out_len, want);
				if (rc <= 0) {
					// It shrank, or the card failed: the rest as zeros, and
					// an error in place of the final 0 byte, as rcp did.
					memset(s->x.out + s->x.out_len, 0, want);
					rc = (int32_t)want;
					s->sent += want;
					s->x.out_len += want;
					if (s->sent == s->info.size) {
						char m[XFER_PATH + 32];
						nsfs_close(s->fd);
						snprintf(m, sizeof(m), "%s: read failed", s->item);
						error1(s, m);
						src_wait(s, A_E);         // its acknowledgement, then on
					}
					budget--;
					break;
				}
				s->sent += (uint32_t)rc;
				s->x.out_len += (uint32_t)rc;
			}
			if (s->sent == s->info.size) {
				nsfs_close(s->fd);
				xfer_emit(&s->x, "", 1);
				src_wait(s, A_DATA_END);
			}
			budget--;
			break;
		}
		}
	}
	return used;
}

// -- the command line --

// Splits a command into words, as a shell would: spaces separate,
// '...' and "..." quote, a backslash escapes. NUL-separated into out.
static int split(const char *cmd, char *out, uint32_t cap, uint32_t *len) {
	uint32_t n = 0;
	int words = 0;
	const char *p = cmd;
	while (*p) {
		while (*p == ' ' || *p == '\t') p++;
		if (!*p) break;
		while (*p && *p != ' ' && *p != '\t') {
			char q = 0;
			if (*p == '\'' || *p == '"') q = *p++;
			if (q) {
				while (*p && *p != q) {
					if (q == '"' && *p == '\\' && p[1]) p++;
					if (n + 1 >= cap) return -1;
					out[n++] = *p++;
				}
				if (*p != q) return -1;
				p++;
			} else {
				if (*p == '\\' && p[1]) p++;
				if (n + 1 >= cap) return -1;
				out[n++] = *p++;
			}
		}
		out[n++] = 0;
		words++;
	}
	*len = n;
	return words;
}

static uint32_t scp_step(xfer_t *x, int budget) {
	scp_t *s = (scp_t *)x;
	uint32_t used = s->mode == SINK ? sink_step(s, budget) : source_step(s, budget);
	// The client sent EOF: a sink is done once what came before it is used.
	if (s->mode == SINK && x->eof && !x->in_len && !x->done) {
		if (s->ks == K_DATA || s->ks == K_DATA_END) {
			xfer_part_abort(s->fd, s->fpath);
			xfer_logf(x, "scp: %s not received in full -- the old file is as it was", s->fpath);
			s->errors++;
			s->ks = K_HEADER;
		}
		x->done = true;
		x->status = s->errors ? 1 : 0;
	}
	return used;
}

static void scp_abort(xfer_t *x) {
	scp_t *s = (scp_t *)x;
	if (s->mode == SINK && (s->ks == K_DATA || s->ks == K_DATA_END)) {
		xfer_part_abort(s->fd, s->fpath);
		xfer_logf(x, "scp: %s not received in full -- the old file is as it was", s->fpath);
		s->ks = K_HEADER;
	}
	if (s->mode == SOURCE && (s->rs == R_DATA || (s->rs == R_ACK &&
			(s->after == A_C || (s->after == A_T && s->info.type != Z_FS_TYPE_DIR))))) {
		nsfs_close(s->fd);
		s->rs = R_NEXT;
	}
}

xfer_t *xfer_scp(const char *cmd, const char *home, uint32_t flags, xfer_log_fn log,
		void *user, char *why, uint32_t why_cap) {
	char words[ARGS_MAX];
	uint32_t wl;
	int nw = split(cmd, words, sizeof(words), &wl);
	scp_t *s;

	if (nw < 2 || strcmp(words, "scp")) { snprintf(why, why_cap, "not an scp command"); return NULL; }
	s = calloc(1, sizeof(*s));
	if (!s) { snprintf(why, why_cap, "out of memory"); return NULL; }
	xfer_base_init(&s->x, home, flags, log, user);
	s->x.step = scp_step;
	s->x.abort = scp_abort;

	const char *w = words + strlen(words) + 1;
	const char *end = words + wl;
	bool opts = true;
	for (; w < end; w += strlen(w) + 1) {
		if (opts && w[0] == '-' && w[1]) {
			if (!strcmp(w, "--")) { opts = false; continue; }
			for (const char *o = w + 1; *o; o++) {
				switch (*o) {
				case 't': s->mode = SINK; break;
				case 'f': s->mode = SOURCE; break;
				case 'r': s->recurse = true; break;
				case 'p': s->times = true; break;
				case 'd': s->want_dir = true; break;
				case 'v': case 'q': case 'E': break;
				default:
					snprintf(why, why_cap, "scp: option -%c is not supported here", *o);
					free(s);
					return NULL;
				}
			}
			continue;
		}
		opts = false;
		uint32_t l = (uint32_t)strlen(w) + 1;
		if (s->args_len + l > sizeof(s->args)) { snprintf(why, why_cap, "scp: too many paths"); free(s); return NULL; }
		memcpy(s->args + s->args_len, w, l);
		s->args_len += l;
	}
	if (!s->mode || !s->args_len) {
		snprintf(why, why_cap, "scp: needs -t or -f and a path");
		free(s);
		return NULL;
	}
	if (s->mode == SINK) {
		z_fs_info_t fi;
		if (s->args_len != strlen(s->args) + 1) { snprintf(why, why_cap, "scp: one target only"); free(s); return NULL; }
		if (!xfer_path(s->target, home, s->args, (uint32_t)strlen(s->args))) {
			snprintf(why, why_cap, "scp: %.200s: Name too long", s->args);
			free(s);
			return NULL;
		}
		s->target_dir = nsfs_stat(s->target, &fi) >= 0 && fi.type == Z_FS_TYPE_DIR;
		s->ks = K_START;
		if (s->want_dir && !s->target_dir) {
			char m[XFER_PATH + 32];
			snprintf(m, sizeof(m), "%s: Not a directory", s->target);
			fatal(s, m);
		}
	} else {
		s->rs = R_WAIT_GO;
	}
	xfer_logf(&s->x, "scp: %s %s", s->mode == SINK ? "receiving into" : "sending", s->mode == SINK ? s->target : s->args);
	return &s->x;
}
