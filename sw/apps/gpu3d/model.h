#ifndef GPU3D_MODEL_H
#define GPU3D_MODEL_H

/*
 * Zeitlos -- gpu3d
 *
 * The wireframe model gpu3d renders, and the fixed-point type it is
 * expressed in. Shared between gpu3d.c (which rotates, projects and
 * draws it) and stl.c (which builds one from a file on the SD card).
 *
 * -- why there is a hard ceiling on size --
 *
 * MODEL_MAX_VERTS/MODEL_MAX_EDGES below are not a limit on what file
 * can be OPENED -- stl.c streams a file of any size and decimates it
 * down to fit these (see that file's header comment). They are a
 * limit on what is kept resident, and they are sized against two
 * separate budgets that happen to land in the same place:
 *
 *   MEMORY. Every byte of .bss is a byte of the 1MB main-memory pool
 *   for this process's whole lifetime -- an app's block is sized from
 *   its IMAGE plus its stack tier, and .bss is part of the image (see
 *   docs/boot.md, "Memory budget"). The arrays below come to ~15KB,
 *   which is what a 768/1536 model costs.
 *
 *   TIME. Every edge is two hardware line commands per frame in the
 *   incremental-erase path, or one in the clear-and-redraw path, plus
 *   one rotate+project per vertex. At ~12 MIPS (picorv32, 48MHz,
 *   ~4 cycles/instruction) a 1500-edge model is already down in the
 *   low tens of frames per second. Raising these numbers buys detail
 *   that the frame counter immediately spends.
 *
 * So they are deliberately joint: a model that fits in the memory
 * budget is also roughly a model that renders at an interactive rate.
 * If you raise one, watch the FPS readout in the window's top-left
 * corner -- that is exactly what it is there to tell you.
 */

#include <stdint.h>

/* -- fixed point --
 *
 * Q12 throughout, in an int32_t. Model space is normalized so that
 * the object's largest half-extent is exactly FIXED_ONE (see
 * model_normalize() in stl.h), which is also the half-extent of the
 * built-in cube -- so the projection math in gpu3d.c never needs to
 * know whether it is drawing the cube or a teapot.
 *
 * Products go through int64 and shift back down, which on rv32im is
 * a mulh/mul pair rather than a call into libgcc. Keep it that way:
 * arch.mk builds this tree as rv32im.
 */
#define FIXED_SHIFT       12
#define FIXED_ONE         (1 << FIXED_SHIFT)

typedef int32_t fixed_t;

#define INT_TO_FIXED(x)   ((fixed_t)((x) << FIXED_SHIFT))
#define FIXED_TO_INT(x)   ((int)((x) >> FIXED_SHIFT))
#define FLOAT_TO_FIXED(x) ((fixed_t)((x) * FIXED_ONE))

static inline fixed_t fixed_mul(fixed_t a, fixed_t b) {
	return (fixed_t)(((int64_t)a * (int64_t)b) >> FIXED_SHIFT);
}

/*
 * SETUP-PATH DIVISION ONLY. Do not use this per vertex per frame.
 *
 * The int64 numerator is not optional here -- model_normalize() (stl.h)
 * divides raw STL coordinates, which can be in the thousands (a model
 * in millimetres), and those overflow a 32-bit numerator once shifted
 * left by FIXED_SHIFT. But rv32im has no 64-bit divide, so this
 * compiles to a call to libgcc's __divdi3: several hundred cycles,
 * against roughly 32 for the hardware DIV instruction.
 *
 * That was fine when this ran once over 768 vertices at load. It was
 * not fine in project(), which called it TWICE PER VERTEX PER FRAME --
 * see recip_q18() in gpu3d.c for the 32-bit reciprocal that replaced
 * it there.
 */
static inline fixed_t fixed_div(fixed_t a, fixed_t b) {
	if (b == 0) return 0;
	return (fixed_t)(((int64_t)a << FIXED_SHIFT) / b);
}

/* -- the model -- */

typedef struct {
	fixed_t x, y, z;
} vertex3d_t;

/*
 * A wireframe edge, as a pair of indices into model_t.verts.
 *
 * uint16_t rather than int specifically because there are up to
 * MODEL_MAX_EDGES of these: at two ints apiece this array would be
 * 12KB instead of 6KB, for a range (65535) that is already 85x more
 * than MODEL_MAX_VERTS can ever need.
 */
typedef struct {
	uint16_t v0, v1;
} medge_t;

/*
 * A triangular face, as three indices into model_t.verts.
 *
 * OPTIONAL. A model may have edges and no faces -- that is what every
 * model here was until shading existed, and what an STL import still
 * produces today. ntris == 0 means "wireframe only", and the renderer
 * falls back to it rather than refusing to draw. Nothing has to be
 * converted or synthesised: a model without faces simply cannot be
 * shaded, which is the honest answer.
 *
 * Winding is COUNTER-CLOCKWISE when the face is seen from outside, so
 * the sign of the projected 2D cross product is what backface culling
 * tests. That convention has to hold for every model that wants
 * shading; a face wound the other way is culled when it should be
 * drawn, which looks like a hole rather than like a winding error.
 */
typedef struct {
	uint16_t v0, v1, v2;
} mtri_t;

/*
 * Sized for INTERACTIVE RATE, not for fidelity.
 *
 * These were 768/1536 and measured 4 FPS on real hardware with a
 * teapot. Profiling (see docs/gpu3d_app.md) put almost none of that in
 * the GPU line rasterizer -- at ~6 cycles per pixel it was busy under
 * 1% of each frame -- and almost all of it in CPU-side per-edge cost:
 * twelve uncached MMIO register writes per line, times 1428 edges,
 * times every frame.
 *
 * That cost is linear in EDGE COUNT and essentially nothing else, so
 * the edge budget is the frame-rate dial. 384 is a quarter of what it
 * was, for roughly four times the frame rate.
 *
 * Raise them and watch the FPS readout and the per-edge figure in the
 * serial-console report -- that is exactly what those are for. Note
 * the vertex budget follows the edge budget rather than leading it: a
 * cluster-decimated closed surface comes out around 0.34 vertices per
 * edge (measured: 490/1428 and 307/911 on two real teapots), so 256
 * against 384 edges leaves comfortable headroom without wasting .bss.
 */
#define MODEL_MAX_VERTS   256
#define MODEL_MAX_EDGES   384

/* Faces are OPTIONAL and only the built-in solids have them today, so
 * this is sized for those rather than for an imported mesh. A cube is
 * 12 triangles; room for a few more built-ins costs 6 bytes each.
 *
 * Deliberately NOT sized to MODEL_MAX_EDGES. A closed mesh has roughly
 * two triangles per three edges, so matching them would add several
 * kilobytes of .bss to every gpu3d build for data that STL import does
 * not currently produce. When it does, this is the number to raise --
 * and the interactive-rate constraint above is what to check it
 * against, not the memory. */
#define MODEL_MAX_TRIS    64

#define MODEL_NAME_MAX    24

typedef struct {

	vertex3d_t	verts[MODEL_MAX_VERTS];
	medge_t		edges[MODEL_MAX_EDGES];

	int		nverts;
	int		nedges;

	/* Faces, if this model has them. 0 for a wireframe-only model --
	 * see mtri_t. */
	mtri_t		tris[MODEL_MAX_TRIS];
	int		ntris;

	/* short display name -- the basename of the loaded file, or
	 * "cube" for the built-in. Shown in the titlebar. */
	char		name[MODEL_NAME_MAX];

	/* true if this model was decimated on load, i.e. what is in
	 * verts/edges is a reduced version of what was in the file.
	 * gpu3d.c says so in the titlebar, because a teapot that is
	 * visibly coarser than the file it came from should not look
	 * like a parsing bug. */
	int		decimated;

} model_t;

#endif
