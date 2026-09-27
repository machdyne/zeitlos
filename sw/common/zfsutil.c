/*
 * Zeitlos
 *
 * File operations built from the zfsapp.h calls: copy, move (rename,
 * or copy and unlink across volumes), and error text. Declared in
 * zfsapp.h; docs/filesystem.md, "Rename, stat and the extended
 * listing".
 *
 * A separate file from zfsapp.c because nothing here makes a syscall
 * of its own. sw/apps/posix/tests links it against host stubs of the
 * zfsapp calls, so the host tests exercise this code, not a copy.
 * An app that uses these links zfsutil.o as well as zfsapp.o.
 */

#include <stdint.h>
#include <string.h>

#include "zfs.h"
#include "zfsapp.h"

static int fs_same_path(const char *a, const char *b) {
	for (;; a++, b++) {
		char x = *a, y = *b;
		if (x >= 'a' && x <= 'z') x -= 32;
		if (y >= 'a' && y <= 'z') y -= 32;
		if (x != y) return 0;
		if (!x) return 1;
	}
}

int fs_copy_file(const char *from, const char *to) {

	// On the stack, not static: an app that copies from two places
	// (a UI thread and a background one) must not share a buffer, and
	// 1KB is a small fraction of any app's stack allowance.
	char buf[1024];
	int in, out, n, ok = 1;

	if (!from || !to || fs_same_path(from, to)) return 0;

	in = fs_open_read(from);
	if (in < 0) return 0;
	out = fs_open_write(to);
	if (out < 0) { fs_close_handle(in); return 0; }

	for (;;) {
		n = fs_read_chunk(in, buf, (int)sizeof(buf));
		if (n < 0) { ok = 0; break; }
		if (n == 0) break;
		if (fs_write_chunk(out, buf, n) != n) { ok = 0; break; }
	}

	fs_close_handle(in);
	fs_close_handle(out);
	if (!ok) fs_unlink((char *)to);
	return ok;

}

int fs_move(const char *from, const char *to, int *err) {

	int e;

	if (fs_rename(from, to, &e)) { if (err) *err = 0; return 1; }
	if (e != Z_FS_ERR_XDEV) { if (err) *err = e; return 0; }

	// Different volumes. Only a file can be moved this way; moving a
	// whole directory tree across volumes is a recursive copy, which
	// is the caller's to decide on.
	{
		z_fs_info_t fi;
		if (fs_stat(to, &fi)) { if (err) *err = Z_FS_ERR_EXIST; return 0; }
		if (!fs_stat(from, &fi)) { if (err) *err = Z_FS_ERR_NOENT; return 0; }
		if (fi.type == Z_FS_TYPE_DIR) { if (err) *err = Z_FS_ERR_XDEV; return 0; }
	}
	if (!fs_copy_file(from, to)) { if (err) *err = Z_FS_ERR_IO; return 0; }
	if (!fs_unlink((char *)from)) { if (err) *err = Z_FS_ERR_BUSY; return 0; }
	if (err) *err = 0;
	return 1;

}

const char *fs_strerror(int err) {
	switch (err) {
	case Z_FS_ERR_NONE:  return "ok";
	case Z_FS_ERR_NOENT: return "no such file or directory";
	case Z_FS_ERR_EXIST: return "destination exists";
	case Z_FS_ERR_XDEV:  return "on a different volume";
	case Z_FS_ERR_BUSY:  return "in use";
	case Z_FS_ERR_INVAL: return "invalid name";
	default:             return "i/o error";
	}
}

