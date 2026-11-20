// zvenc_core.h -- the ZV base-layer encoder, without any I/O.
//
// One grey frame in, one record's video payload out. The encoder's
// idea of the picture is made by decoding its own payload with zv.c,
// so the encoder and every player agree to the pixel.

#ifndef ZVENC_CORE_H
#define ZVENC_CORE_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
	int w, h;                   // multiples of 8
	double lambda;              // distortion (grey^2) a bit; 0: from rate
	double target_bytes;        // video bytes a frame for rate control (0: fixed lambda)
	double target_cost;         // and what a frame may cost the board to decode, in
	                            // zve_stats_t's `cost` (0: no limit): rate control
	                            // holds whichever of the two is nearer its limit
	int iters;                  // k-means passes a frame (inter); key frames get more
	int key_iters;
	double key_lambda_mul;      // seek points coded this much coarser (default: not)
	int refresh;                // frames to refresh the picture after a seek point
	int threads;                // 0: one a processor
	int motion_probe;           // measure what motion blocks would save
	int verbose;
} zve_opts_t;

// A frame's cost to the board's decoder, in instructions (the fast
// loops of sw/common/zv.c; the proportions counted, the scale fitted to
// what zsys ran for a clip; docs/zv.md, "A frame's cost").
#define ZVE_COST_FRAME  8750.0
#define ZVE_COST_V1     77.0
#define ZVE_COST_V4     168.0
#define ZVE_COST_V16    400.0
#define ZVE_COST_UPD    245.0   // a V1 or V4 entry changed: drawn again when next used
#define ZVE_COST_UPD16  49.0

typedef struct {
	uint32_t blocks, skip, v1, v4, v16, rep_blocks;
	uint32_t upd1, upd4, upd16;
	uint32_t bytes;
	double sse;                 // against the source
	double lambda;
	double cost;                // the board's decode time for this frame, estimated:
	                            // instructions (docs/zv.md, "A frame's cost")
	uint32_t probe_blocks;      // so far: blocks a motion copy would have coded more cheaply
	double probe_bits;          // so far: the bits that would have saved
} zve_stats_t;

typedef struct zve zve_t;

zve_t *zve_new(const zve_opts_t *o);
void zve_free(zve_t *e);

// Encode one frame. `grey` is w x h, 8 bits. `key` forces a keyframe.
// Returns the payload length (0 on error); *stats if not NULL.
size_t zve_frame(zve_t *e, const uint8_t *grey, int key, uint8_t *out, size_t cap,
                 zve_stats_t *stats);

// The encoder's (and so every decoder's) picture after the last frame:
// w x h grey.
const uint8_t *zve_recon(const zve_t *e);

// Worst-case payload size for w x h.
size_t zve_max_payload(int w, int h);

#endif
