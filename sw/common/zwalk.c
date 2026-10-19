/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * A folder walk in small steps: find, copy, delete. See zwalk.h and
 * docs/zwalk.md. No printf, no malloc: the caller owns all memory.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "zfs.h"
#include "zfsapp.h"
#include "zwalk.h"

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

bool zw_name_match(const char *pat, bool glob, const char *name) {

	if (!pat[0]) return true;

	if (!glob) {
		// a part of the name
		for (const char *s = name; *s; s++) {
			int i = 0;
			while (pat[i] && s[i] && lower(pat[i]) == lower(s[i])) i++;
			if (!pat[i]) return true;
		}
		return false;
	}

	// * and ?, with one point to come back to: the classic loop that
	// needs no recursion and no stack for a pattern of any shape.
	const char *p = pat, *n = name, *star = NULL, *back = NULL;
	while (*n) {
		if (*p == '*') { star = ++p; back = n; continue; }
		if (*p && (*p == '?' || lower(*p) == lower(*n))) { p++; n++; continue; }
		if (!star) return false;
		p = star;
		n = ++back;
	}
	while (*p == '*') p++;
	return !*p;

}

bool zw_path_inside(const char *inner, const char *outer) {
	int n = 0;
	while (outer[n] && lower(outer[n]) == lower(inner[n])) n++;
	if (outer[n]) return false;
	if (n > 0 && outer[n - 1] == '/') return true;		// outer is "/"
	return inner[n] == 0 || inner[n] == '/';
}

const char *zw_strerror(int err) {
	switch (err) {
	case ZW_E_NOTFOUND: return "not found";
	case ZW_E_READ:     return "a file could not be read";
	case ZW_E_WRITE:    return "a file could not be written (full?)";
	case ZW_E_MKDIR:    return "a folder could not be made";
	case ZW_E_DELETE:   return "something could not be deleted (in use?)";
	case ZW_E_INSIDE:   return "the destination is inside the folder";
	case ZW_E_MEMORY:   return "not enough memory";
	case ZW_E_EXISTS:   return "the destination is already there";
	default:            return "ok";
	}
}

// -- paths --

static int slen(const char *s) { return (int)strlen(s); }

// Appends "/name" to `p` (at `len`), unless that would not fit.
static bool path_add(char *p, const char *name) {
	int n = slen(p), l = slen(name);
	bool slash = !(n > 0 && p[n - 1] == '/');
	if (n + (slash ? 1 : 0) + l + 1 > Z_FS_PATH_MAX) return false;
	if (slash) p[n++] = '/';
	memcpy(p + n, name, (size_t)l + 1);
	return true;
}

static const char *base_of(const char *p) {
	const char *b = p;
	for (; *p; p++) if (*p == '/') b = p + 1;
	return b;
}

// Back to this level's paths, after an entry has been dealt with.
static void restore(zwalk_t *w) {
	w->src[w->st[w->depth].slen] = 0;
	if (w->op == ZW_COPY) w->dst[w->st[w->depth].dlen] = 0;
}

static int fail(zwalk_t *w, int err) {
	int i;
	for (i = 0; w->src[i] && i < Z_FS_PATH_MAX - 1; i++) w->hit[i] = w->src[i];
	w->hit[i] = 0;
	w->err = err;
	zw_cancel(w);
	return ZW_FAIL;
}

static int hit(zwalk_t *w) {
	memcpy(w->hit, w->src, (size_t)slen(w->src) + 1);
	return ZW_HIT;
}

// -- setup --

bool zw_start(zwalk_t *w, int op, const char *src, const char *dst,
	void *mem, uint32_t memcap) {

	uint32_t bufsz = ZW_CHUNK + ZW_TEXT_MAX;
	z_fs_info_t fi;

	memset(w, 0, sizeof(*w));
	w->op = op;
	w->in = w->out = -1;

	if (!mem || memcap < bufsz + 1024) { w->err = ZW_E_MEMORY; return false; }
	w->buf = (uint8_t *)mem;
	w->maxent = (memcap - bufsz) / 24;
	if (w->maxent > 512) w->maxent = 512;
	w->types = w->buf + bufsz;
	w->pool = (char *)w->types + w->maxent;
	w->poolcap = memcap - bufsz - w->maxent;

	if (slen(src) >= Z_FS_PATH_MAX || (dst && slen(dst) >= Z_FS_PATH_MAX)) {
		w->err = ZW_E_NOTFOUND;
		return false;
	}
	memcpy(w->src, src, (size_t)slen(src) + 1);
	// "/home/" walks as "/home"; "/" stays.
	while (slen(w->src) > 1 && w->src[slen(w->src) - 1] == '/')
		w->src[slen(w->src) - 1] = 0;

	if (!fs_stat(w->src, &fi)) {
		// The root of a volume has no entry to stat; it lists.
		uint32_t n = 0, t = 0;
		if (!fs_list_into(w->src, w->pool, w->poolcap, w->types,
			w->maxent, &n, &t) && slen(w->src) > 1) {
			w->err = ZW_E_NOTFOUND;
			return false;
		}
		fi.type = Z_FS_TYPE_DIR;
	}
	w->root_file = fi.type != Z_FS_TYPE_DIR;

	if (op == ZW_COPY) {
		if (!dst || !dst[0]) { w->err = ZW_E_NOTFOUND; return false; }
		memcpy(w->dst, dst, (size_t)slen(dst) + 1);
		if (fs_stat(w->dst, &fi)) { w->err = ZW_E_EXISTS; return false; }
		if (!w->root_file && zw_path_inside(w->dst, w->src)) {
			w->err = ZW_E_INSIDE;
			return false;
		}
	}

	w->st[0].slen = (uint16_t)slen(w->src);
	w->st[0].dlen = (uint16_t)slen(w->dst);
	return true;

}

void zw_find_set(zwalk_t *w, const char *name, const char *text) {
	int i;
	w->name_glob = false;
	for (i = 0; name && name[i] && i < ZW_NAME_MAX - 1; i++) {
		w->name[i] = name[i];
		if (name[i] == '*' || name[i] == '?') w->name_glob = true;
	}
	w->name[i] = 0;
	for (i = 0; text && text[i] && i < ZW_TEXT_MAX - 1; i++) w->text[i] = text[i];
	w->text[i] = 0;
	w->tlen = i;
}

// -- a file --

// Starts on the file at w->src (and w->dst): one call or none.
static int file_begin(zwalk_t *w) {

	switch (w->op) {

	case ZW_FIND:
		if (!zw_name_match(w->name, w->name_glob, base_of(w->src))) return ZW_MORE;
		w->files++;
		if (!w->tlen) return hit(w);
		w->in = fs_open_read(w->src);
		if (w->in < 0) { w->skipped++; return ZW_MORE; }
		w->carry = 0;
		w->infile = true;
		return ZW_MORE;

	case ZW_COPY:
		w->in = fs_open_read(w->src);
		if (w->in < 0) return fail(w, ZW_E_READ);
		w->out = fs_open_write(w->dst);
		if (w->out < 0) return fail(w, ZW_E_WRITE);
		w->infile = true;
		return ZW_MORE;

	default:	// ZW_DELETE
		if (!fs_unlink(w->src)) return fail(w, ZW_E_DELETE);
		w->files++;
		return ZW_MORE;

	}

}

static void file_end(zwalk_t *w) {
	if (w->in >= 0) fs_close_handle(w->in);
	if (w->out >= 0) fs_close_handle(w->out);
	w->in = w->out = -1;
	w->infile = false;
}

// Is the find's text in buf[0..n)? ASCII case folded.
static bool text_in(const zwalk_t *w, int n) {
	const uint8_t *b = w->buf;
	for (int i = 0; i + w->tlen <= n; i++) {
		int k = 0;
		while (k < w->tlen && lower((char)b[i + k]) == lower(w->text[k])) k++;
		if (k == w->tlen) return true;
	}
	return false;
}

// One chunk of the file in progress.
static int file_step(zwalk_t *w) {

	if (w->op == ZW_COPY) {
		int n = fs_read_chunk(w->in, w->buf, ZW_CHUNK);
		if (n < 0) return fail(w, ZW_E_READ);
		if (n == 0) {
			file_end(w);
			w->files++;
			w->partial = true;
			restore(w);
			return ZW_MORE;
		}
		if (fs_write_chunk(w->out, w->buf, n) != n) return fail(w, ZW_E_WRITE);
		w->bytes += (uint32_t)n;
		return ZW_MORE;
	}

	// find: the text, across chunk boundaries -- the last tlen-1
	// bytes of one chunk are kept in front of the next.
	int n = fs_read_chunk(w->in, w->buf + w->carry, ZW_CHUNK);
	if (n <= 0) {
		if (n < 0) w->skipped++;
		file_end(w);
		restore(w);
		return ZW_MORE;
	}
	w->bytes += (uint32_t)n;
	n += w->carry;
	if (text_in(w, n)) {
		file_end(w);
		int r = hit(w);
		restore(w);
		return r;
	}
	w->carry = w->tlen - 1 < n ? w->tlen - 1 : n;
	memmove(w->buf, w->buf + n - w->carry, (size_t)w->carry);
	return ZW_MORE;

}

// -- the walk --

int zw_step(zwalk_t *w) {

	if (w->err) return ZW_FAIL;
	if (w->op < 0) return ZW_DONE;		// finished or cancelled

	if (w->infile) return file_step(w);

	if (w->root_file) {
		if (w->listed) { w->op = -1; return ZW_DONE; }
		w->listed = true;		// "begun", for a file
		return file_begin(w);
	}

	zw_frame_t *f = &w->st[w->depth];

	// copy: the folder itself, first
	if (w->op == ZW_COPY && w->depth == 0 && !w->partial) {
		if (!fs_mkdir(w->dst)) return fail(w, ZW_E_MKDIR);
		w->partial = true;
		w->dirs++;
		return ZW_MORE;
	}

	if (!w->listed) {
		uint32_t t = 0;
		// An empty folder lists as a failure (zflist.c says why);
		// either way there is nothing in it to walk.
		if (!fs_list_into(w->src, w->pool, w->poolcap, w->types,
			w->maxent, &w->count, &t))
			w->count = 0;
		w->list_trunc = t != 0;
		if (t) w->truncated = true;
		w->cur = w->off = 0;
		w->listed = true;
		return ZW_MORE;
	}

	if (f->idx < w->count) {

		while (w->cur < f->idx) {
			w->off += (uint32_t)slen(w->pool + w->off) + 1;
			w->cur++;
		}
		const char *name = base_of(w->pool + w->off);
		bool isdir = w->types[f->idx] == Z_FS_TYPE_DIR;
		f->idx++;

		if (!name[0] || !path_add(w->src, name) ||
			(w->op == ZW_COPY && !path_add(w->dst, name))) {
			w->skipped++;
			restore(w);
			return ZW_MORE;
		}

		if (!isdir) {
			int r = file_begin(w);
			if (!w->infile && r != ZW_FAIL) restore(w);
			return r;
		}

		if (w->depth == ZW_DEPTH) {
			w->skipped++;
			restore(w);
			return ZW_MORE;
		}

		w->depth++;
		w->st[w->depth].idx = 0;
		w->st[w->depth].slen = (uint16_t)slen(w->src);
		w->st[w->depth].dlen = (uint16_t)slen(w->dst);
		w->listed = false;

		if (w->op == ZW_COPY) {
			if (!fs_mkdir(w->dst)) return fail(w, ZW_E_MKDIR);
			w->dirs++;
			return ZW_MORE;
		}
		if (w->op == ZW_FIND && !w->tlen &&
			zw_name_match(w->name, w->name_glob, name)) {
			w->dirs++;
			return hit(w);
		}
		return ZW_MORE;

	}

	// The folder is done.
	if (w->op == ZW_DELETE) {
		if (w->list_trunc) {			// there is more of it: again
			w->listed = false;
			f->idx = 0;
			return ZW_MORE;
		}
		if (!fs_unlink(w->src)) return fail(w, ZW_E_DELETE);
		w->dirs++;
	}

	if (w->depth == 0) { w->op = -1; return ZW_DONE; }

	w->depth--;
	restore(w);
	w->listed = false;
	if (w->op == ZW_DELETE) w->st[w->depth].idx = 0;	// what came before is gone
	return ZW_MORE;

}

void zw_cancel(zwalk_t *w) {
	bool writing = w->infile && w->out >= 0;
	file_end(w);
	// The file being written is not kept: half a file looks like a
	// whole one. Folders and files already finished stay (partial).
	if (writing) fs_unlink(w->dst);
	if (w->op >= 0) w->op = -1;
}
