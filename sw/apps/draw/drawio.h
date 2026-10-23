#ifndef DRAWIO_H
#define DRAWIO_H

/*
 * draw -- reading and writing the canvas file a slice at a time, from
 * the main loop (docs/responsive.md, docs/draw_app.md).
 *
 *   draw_io_t io;
 *   draw_io_load_start(&io, path, spare, W, H);   or  draw_io_save_start(...)
 *   main loop, nothing waiting:
 *       if (draw_io_step(&io, Z_TICK_HZ / 50) != DRAW_IO_RUNNING) ...done
 *       show draw_io_permille(&io)
 *   Esc:  draw_io_stop(&io)       file closed (a save's .part deleted)
 *
 * A load reads into a buffer the caller gives it, never into the canvas
 * on screen: the caller swaps the two only once the load is DONE, so a
 * stop or a damaged file leaves the drawing as it was.
 *
 * A save writes PATH.part and, once every byte is written and the file
 * closed, renames it to PATH (removing the old PATH first only if the
 * rename refuses to replace it). A stopped or failed save deletes the
 * .part and leaves the old PATH untouched. The canvas must not change
 * while a save runs: the caller keeps it read-only.
 *
 * Both are plain state machines: no coroutine, no malloc; a job holds
 * at most one open file, and nothing once it is not RUNNING.
 */

#include <stdint.h>
#include <stdbool.h>

#include "../../common/zfs.h"	// Z_FS_PATH_MAX

enum {
	DRAW_IO_IDLE = 0,
	DRAW_IO_RUNNING,
	DRAW_IO_DONE,
	DRAW_IO_FAILED,
	DRAW_IO_STOPPED,
};

enum {
	DRAW_IO_LOAD = 1,
	DRAW_IO_SAVE,
};

// FAILED: why (draw_io_error() says it in words)
enum {
	DRAW_IO_E_NONE = 0,
	DRAW_IO_E_OPEN,		// load: missing/unreadable; save: cannot create the .part
	DRAW_IO_E_NOTZBM,	// not a ZBM (and not a headerless canvas either)
	DRAW_IO_E_SIZE,		// a ZBM, but not the canvas's size
	DRAW_IO_E_SHORT,	// the file ends early: damaged
	DRAW_IO_E_READ,		// a read failed
	DRAW_IO_E_WRITE,	// a write (or the close that flushes it) failed
	DRAW_IO_E_RENAME,	// written, but the .part could not become the file
	DRAW_IO_E_KEPT,		// the old file is gone and the new one is still NAME.part
	DRAW_IO_E_COLOUR,	// a colour picture (planes in the flags, zbm.h): pix's
};

#define DRAW_IO_CHUNK	2048		// bytes a read or write

typedef struct {
	int			kind;			// DRAW_IO_LOAD / _SAVE
	int			state;
	int			err;
	char		path[Z_FS_PATH_MAX];
	char		part[Z_FS_PATH_MAX + 8];	// save: path + ".part"
	bool		legacy;			// load: a headerless file from before ZBM

	// -- inside --
	int			fh;				// -1 when closed
	uint8_t		*buf;			// load: where it goes; save: what is written
	uint32_t	w, h, bytes;	// the canvas, and its pixel bytes
	uint32_t	size;			// load: the file's
	bool		hdr;			// the header is read / written
	uint32_t	done;			// pixel bytes read / written
} draw_io_t;

// Start a load of `path` into `dst` (w x h, z_bm_data_size() bytes).
// False, with state FAILED, if the file cannot be opened.
bool draw_io_load_start(draw_io_t *io, const char *path, void *dst,
	uint32_t w, uint32_t h);

// Start a save of `src` to `path` (through path.part). False, with
// state FAILED, if the .part cannot be created.
bool draw_io_save_start(draw_io_t *io, const char *path, const void *src,
	uint32_t w, uint32_t h);

// About `ticks` of work (at least one chunk), then its state.
int draw_io_step(draw_io_t *io, uint32_t ticks);

// 0..1000
uint16_t draw_io_permille(const draw_io_t *io);

// Stop now: the file closed; a save's .part deleted. STOPPED. Nothing
// if it is not RUNNING.
void draw_io_stop(draw_io_t *io);

bool draw_io_running(const draw_io_t *io);

// Two short lines for a dialog, for a FAILED job's err.
const char *draw_io_error(int err);

#endif
