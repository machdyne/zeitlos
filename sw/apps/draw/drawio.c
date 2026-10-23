/*
 * draw -- the canvas file, a slice at a time. See drawio.h.
 */

#include "drawio.h"

#include <string.h>

#include "../../common/zeitlos.h"	// z_uptime_ticks()
#include "../../common/zfsapp.h"
#include "../../common/zbm.h"

static void copy_str(char *d, int n, const char *s) {
	int i = 0;
	for (; s[i] && i < n - 1; i++) d[i] = s[i];
	d[i] = 0;
}

static void begin(draw_io_t *io, int kind, const char *path, void *buf,
	uint32_t w, uint32_t h) {
	memset(io, 0, sizeof(*io));
	io->kind = kind;
	io->state = DRAW_IO_RUNNING;
	io->fh = -1;
	io->buf = buf;
	io->w = w;
	io->h = h;
	io->bytes = z_bm_data_size(w, h);
	copy_str(io->path, (int)sizeof(io->path), path);
}

static void close_file(draw_io_t *io) {
	if (io->fh >= 0) fs_close_handle(io->fh);
	io->fh = -1;
}

static int fail(draw_io_t *io, int err) {
	close_file(io);
	if (io->kind == DRAW_IO_SAVE && err != DRAW_IO_E_KEPT)
		fs_unlink(io->part);
	io->err = err;
	io->state = DRAW_IO_FAILED;
	return io->state;
}

bool draw_io_load_start(draw_io_t *io, const char *path, void *dst,
	uint32_t w, uint32_t h) {

	int size;

	begin(io, DRAW_IO_LOAD, path, dst, w, h);

	size = fs_size(io->path);
	if (size <= 0) { fail(io, DRAW_IO_E_OPEN); return false; }
	io->size = (uint32_t)size;

	io->fh = fs_open_read(io->path);
	if (io->fh < 0) { fail(io, DRAW_IO_E_OPEN); return false; }

	return true;
}

bool draw_io_save_start(draw_io_t *io, const char *path, const void *src,
	uint32_t w, uint32_t h) {

	int n;

	begin(io, DRAW_IO_SAVE, path, (void *)src, w, h);

	n = (int)strlen(io->path);
	if (n == 0 || n + 6 > Z_FS_PATH_MAX) {
		io->err = DRAW_IO_E_OPEN;
		io->state = DRAW_IO_FAILED;
		return false;
	}
	memcpy(io->part, io->path, (size_t)n);
	memcpy(io->part + n, ".part", 6);

	io->fh = fs_open_write(io->part);
	if (io->fh < 0) { fail(io, DRAW_IO_E_OPEN); return false; }

	return true;
}

// The header: read and checked (load), or written (save).
static int do_header(draw_io_t *io) {

	z_bm_header_t hdr;

	if (io->kind == DRAW_IO_SAVE) {
		z_bm_header_init(&hdr, io->w, io->h);
		if (fs_write_chunk(io->fh, &hdr, Z_BM_HEADER_SIZE) != Z_BM_HEADER_SIZE)
			return fail(io, DRAW_IO_E_WRITE);
		io->hdr = true;
		return io->state;
	}

	if (fs_read_chunk(io->fh, &hdr, Z_BM_HEADER_SIZE) != Z_BM_HEADER_SIZE)
		return fail(io, io->size < Z_BM_HEADER_SIZE ? DRAW_IO_E_NOTZBM : DRAW_IO_E_READ);

	if (!z_bm_header_valid(&hdr)) {
		// a headerless canvas from before ZBM: only its length says so
		if (!z_bm_is_legacy_size(io->size, io->w, io->h))
			return fail(io, DRAW_IO_E_NOTZBM);
		if (!fs_seek(io->fh, 0)) return fail(io, DRAW_IO_E_READ);
		io->legacy = true;
	} else if (hdr.flags != 0) {
		// a colour picture: draw is one bit a pixel
		return fail(io, DRAW_IO_E_COLOUR);
	} else if (hdr.width != io->w || hdr.height != io->h) {
		return fail(io, DRAW_IO_E_SIZE);
	}

	io->hdr = true;
	return io->state;
}

// Written and closed: the .part becomes the file.
static int finish_save(draw_io_t *io) {

	int err = 0;
	int ok = fs_close_handle(io->fh);

	io->fh = -1;
	if (!ok) return fail(io, DRAW_IO_E_WRITE);

	if (!fs_rename(io->part, io->path, &err)) {
		if (err != Z_FS_ERR_EXIST) return fail(io, DRAW_IO_E_RENAME);
		// The old file has to go first. If it will not, nothing is lost:
		// the .part goes instead.
		if (!fs_unlink(io->path)) return fail(io, DRAW_IO_E_RENAME);
		// It went, so now the .part is the only copy: keep it.
		if (!fs_rename(io->part, io->path, &err)) return fail(io, DRAW_IO_E_KEPT);
	}

	io->state = DRAW_IO_DONE;
	return io->state;
}

int draw_io_step(draw_io_t *io, uint32_t ticks) {

	uint32_t t0 = z_uptime_ticks();

	if (io->state != DRAW_IO_RUNNING) return io->state;

	do {
		uint32_t n;
		int r;

		if (!io->hdr) {
			if (do_header(io) != DRAW_IO_RUNNING) return io->state;
			continue;
		}

		n = io->bytes - io->done;
		if (n > DRAW_IO_CHUNK) n = DRAW_IO_CHUNK;

		if (n == 0) {
			if (io->kind == DRAW_IO_SAVE) return finish_save(io);
			close_file(io);
			io->state = DRAW_IO_DONE;
			return io->state;
		}

		if (io->kind == DRAW_IO_SAVE) {
			r = fs_write_chunk(io->fh, io->buf + io->done, (int)n);
			if (r != (int)n) return fail(io, DRAW_IO_E_WRITE);
		} else {
			r = fs_read_chunk(io->fh, io->buf + io->done, (int)n);
			if (r < 0) return fail(io, DRAW_IO_E_READ);
			if (r == 0) return fail(io, DRAW_IO_E_SHORT);
		}
		io->done += (uint32_t)r;

	} while (z_uptime_ticks() - t0 < ticks);

	return io->state;
}

uint16_t draw_io_permille(const draw_io_t *io) {
	if (io->state == DRAW_IO_DONE) return 1000;
	if (!io->bytes) return 0;
	return (uint16_t)(io->done * 999u / io->bytes);	// done <= 38400: no overflow
}

void draw_io_stop(draw_io_t *io) {
	if (io->state != DRAW_IO_RUNNING) return;
	close_file(io);
	if (io->kind == DRAW_IO_SAVE) fs_unlink(io->part);
	io->state = DRAW_IO_STOPPED;
}

bool draw_io_running(const draw_io_t *io) {
	return io->state == DRAW_IO_RUNNING;
}

const char *draw_io_error(int err) {
	switch (err) {
	case DRAW_IO_E_OPEN:	return "That file could not\nbe opened.";
	case DRAW_IO_E_NOTZBM:	return "That file is not\na ZBM bitmap.";
	case DRAW_IO_E_SIZE:	return "That image is not\n640x480.";
	case DRAW_IO_E_SHORT:	return "That file is damaged:\nit ends early.";
	case DRAW_IO_E_READ:	return "That file could not\nbe read.";
	case DRAW_IO_E_WRITE:	return "The image could not be\nwritten completely.";
	case DRAW_IO_E_RENAME:	return "The image was written\nbut could not replace\nthe old file.";
	case DRAW_IO_E_KEPT:	return "The old file is gone;\nthe image is in the\n.part file beside it.";
	case DRAW_IO_E_COLOUR:	return "That is a colour\npicture: open it\nin pix.";
	default:				return "Something went wrong.";
	}
}
