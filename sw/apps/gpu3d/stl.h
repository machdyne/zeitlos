#ifndef GPU3D_STL_H
#define GPU3D_STL_H

/*
 * Zeitlos -- gpu3d
 *
 * Streaming STL loader. See stl.c for the full design writeup; the
 * one thing a caller has to know is in stl_load()'s own comment
 * below: this NEVER holds the file in memory, and it will happily
 * open a file far larger than the machine has RAM for.
 */

#include <stdbool.h>
#include <stdint.h>

#include "model.h"

/*
 * Loads `path` into `*m`, replacing whatever was there.
 *
 * Handles both STL flavours -- binary (84-byte header + 50 bytes per
 * triangle) and ASCII ("solid"/"facet"/"vertex" text) -- detected
 * from the file itself, not from its extension. See stl.c's
 * detect_format() for why the detection is a size check rather than
 * the usual "does it start with the word solid" test, which gets
 * binary files produced by several common exporters wrong.
 *
 * MEMORY: the file is read in 512-byte chunks and never held whole.
 * A 400KB teapot is loaded on a machine whose entire main memory
 * pool is 1MB and whose app heap is 16KB. What IS held is the
 * decimated result, bounded by MODEL_MAX_VERTS/MODEL_MAX_EDGES
 * (model.h) -- so the ceiling on what this can open is the SD card,
 * not RAM.
 *
 * TIME: this makes more than one pass over the file (two normally,
 * occasionally three or four -- see stl.c's grid ladder), so it is
 * seconds, not milliseconds, on a 400KB file. It is a blocking call.
 *
 * `pump` is called periodically while reading -- roughly once per
 * 512-byte chunk -- and MUST service the app's wm message queue,
 * acking Z_WM_REDRAW in particular. It may be NULL only for a caller
 * with no window. This is not optional politeness: wm blocks waiting
 * for a redraw ack and an app that stops acking freezes the whole
 * screen until REDRAW_ACK_TIMEOUT fires (see docs/window_manager.md,
 * "content z-order"), and a multi-second load is more than long
 * enough to hit that.
 *
 * Returns false if the file could not be opened, wasn't recognisable
 * as either STL flavour, or contained no usable triangles. `*m` is
 * left in a defined-but-empty state in that case (nverts/nedges 0),
 * NOT with whatever it held before -- the caller is expected to put
 * something back, which is what gpu3d.c's load_cube() does.
 * stl_last_error() describes the failure.
 */
bool stl_load(const char *path, model_t *m, void (*pump)(void));

/*
 * Where the time went in the last stl_load().
 *
 * This exists because a 400KB teapot took ~60 seconds to load and
 * there was no way to tell, from outside, whether that was the SD
 * card, the parser, or simply the number of times the file was read.
 * Those three have completely different fixes, so guessing between
 * them is wasted work.
 *
 * `io_cycles` is time spent strictly inside fs_read_chunk(); the
 * difference between it and `total_cycles` is parsing, clustering and
 * everything else. `passes` is how many times the file was read end
 * to end -- see stl_load() on why that is normally two and can be
 * three.
 *
 * Cycles rather than milliseconds because this file has no business
 * knowing the clock rate; STL_CYCLES_PER_MS below converts, and
 * matches Z_SYSCLK_HZ (zsoc.h), fixed at 48MHz across every board.
 * They are 64-bit because a slow load can exceed the ~89 seconds a
 * 32-bit cycle count covers at that rate.
 *
 * Valid until the next stl_load(). Never NULL.
 */
typedef struct {

	uint32_t	bytes;		/* total bytes read, all passes */
	uint32_t	passes;		/* times the file was read end to end */
	uint32_t	tris;		/* triangles found in the file */
	int		grid;		/* clustering grid that finally fit */
	int		binary;		/* 1 = binary STL, 0 = ASCII */

	uint64_t	io_cycles;	/* inside fs_read_chunk() */
	uint64_t	total_cycles;	/* inside stl_load() */

	/* The two phases, so a slow load can be blamed on the right one.
	 * Both INCLUDE their share of io_cycles -- they are wall time
	 * spent in that phase, not CPU time. */
	uint64_t	bbox_cycles;	/* pass 1, measuring the bounding box */
	uint64_t	build_cycles;	/* all clustering passes together */

} stl_stats_t;

#define STL_CYCLES_PER_MS   48000u

const stl_stats_t *stl_last_stats(void);

/*
 * A short, human-readable description of why the last stl_load()
 * returned false. Valid until the next call. Never NULL.
 */
const char *stl_last_error(void);

/*
 * Centers `*m` on its own bounding box and scales it so its largest
 * half-extent is exactly FIXED_ONE -- the same size and origin the
 * built-in cube already has, so gpu3d.c's projection needs no
 * per-model tuning.
 *
 * Called by stl_load() itself; exposed because it is what defines
 * the contract the renderer relies on, and anything else that builds
 * a model_t by hand needs to satisfy the same one.
 */
void model_normalize(model_t *m);

#endif
