/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Message history: the file. See mesh_log.h for the format and
 * docs/mesh_app.md, "History".
 *
 * Every write opens, appends and closes. FatFs commits on close, so a
 * record that returned is on the card, and a machine switched off
 * between messages loses nothing; the cost is a few milliseconds per
 * message, which on LoRa is not a rate that matters.
 */

#include <string.h>

#include "zfsapp.h"
#include "mesh_log_io.h"

static bool broken;			// a write failed: stop trying until restart

static bool ensure(const char *path) {
	if (fs_size((char *)path) >= 0) return true;
	fs_mkdir("/user");			// fails harmlessly if it exists
	if (!fs_touch(path)) return false;
	{
		int h = fs_open_rw(path);
		if (h < 0) return false;
		fs_write_chunk(h, MESH_LOG_HEADER, (int)strlen(MESH_LOG_HEADER));
		fs_close_handle(h);
	}
	return true;
}

static bool append(const char *path, const char *rec, int len) {
	int h, size;
	bool ok;
	if (broken || len <= 0) return false;
	if (!ensure(path)) { broken = true; return false; }
	size = fs_size((char *)path);
	h = fs_open_rw(path);
	if (h < 0 || size < 0) { broken = true; return false; }
	ok = fs_seek(h, (uint32_t)size) && fs_write_chunk(h, rec, len) == len;
	fs_close_handle(h);
	if (!ok) broken = true;
	return ok;
}

bool mesh_log_io_ok(void) { return !broken; }

uint32_t mesh_log_load(mesh_model_t *m, const char *path) {
	static mesh_log_rd_t rd;
	char buf[512];
	int size = fs_size((char *)path), h, n;
	uint32_t from = 0;

	if (size <= 0) return 0;
	h = fs_open_read(path);
	if (h < 0) return 0;
	if ((uint32_t)size > MESH_LOG_TAIL_BYTES) {
		from = (uint32_t)size - MESH_LOG_TAIL_BYTES;
		fs_seek(h, from);
	}
	mesh_log_rd_init(&rd);
	// Starting mid-file, the first line is a fragment: skip to the end
	// of it rather than have it counted as a bad record.
	if (from) rd.overlong = 1;
	while ((n = fs_read_chunk(h, buf, sizeof(buf))) > 0) {
		if (from && rd.overlong) {
			int k = 0;
			while (k < n && buf[k] != '\n') k++;
			if (k < n) {
				rd.overlong = 0;
				rd.n = 0;
				mesh_log_feed(&rd, m, buf + k + 1, n - k - 1);
				from = 0;
			}
			continue;
		}
		mesh_log_feed(&rd, m, buf, n);
	}
	mesh_log_finish(&rd, m);
	fs_close_handle(h);
	return rd.loaded;
}

// Past the size limit, rewrite the file as what is held in memory.
static void compact(mesh_model_t *m, const char *path) {
	static char rec[MESH_LOG_LINE_MAX + 2];
	uint32_t i;
	int h = fs_open_write(path);		// truncates
	if (h < 0) { broken = true; return; }
	fs_write_chunk(h, MESH_LOG_HEADER, (int)strlen(MESH_LOG_HEADER));
	for (i = 0; i < m->msg_count; i++) {
		int n = mesh_log_fmt_msg(mesh_msg_at(m, i), rec, sizeof(rec));
		if (n && fs_write_chunk(h, rec, n) != n) { broken = true; break; }
	}
	fs_close_handle(h);
}

bool mesh_log_msg(mesh_model_t *m, const mesh_msg_t *g, const char *path) {
	static char rec[MESH_LOG_LINE_MAX + 2];
	int n = mesh_log_fmt_msg(g, rec, sizeof(rec));
	int size;
	if (!append(path, rec, n)) return false;
	size = fs_size((char *)path);
	if (size > (int)MESH_LOG_MAX_BYTES) compact(m, path);
	return true;
}

bool mesh_log_status(const mesh_msg_t *g, const char *path) {
	char rec[48];
	return append(path, rec, mesh_log_fmt_status(g, rec, sizeof(rec)));
}
