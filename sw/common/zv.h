// zv.h -- ZV, the Zeitlos Video Format: file layout and the decoder.
//
// docs/zv.md is the specification; this header is its C face. The
// decoder (zv.c) is plain C with no allocation and no I/O: the caller
// reads records from the file and hands them over. It builds for the
// board (sw/apps/cinema, to come), for the host encoder
// (tools/zv/zvenc) and for the Linux player (tools/zv/zvplay), so all
// three agree on every pixel.
//
// Draft 0 of the format: the base (grey) layer and 8-bit PCM audio.
// Motion blocks are reserved. The colour layer (ZV_F_COLOUR) is optional.

#ifndef ZV_H
#define ZV_H

#include <stdint.h>
#include <stddef.h>

// ---- file layout (all little-endian) --------------------------------

#define ZV_MAGIC0 'Z'
#define ZV_MAGIC1 'V'
#define ZV_MAGIC2 'F'
#define ZV_MAGIC3 '0'           // draft 0

#define ZV_HDR_SIZE     64
#define ZV_REC_HDR      8       // u32 size, u8 kind, u8 flags, u16 n_audio

#define ZV_BLK          8       // base layer block: 8x8 at full size
#define ZV_MAX_W        640
#define ZV_MAX_H        480
#define ZV_MAX_BW       (ZV_MAX_W / ZV_BLK)
#define ZV_MAX_BH       (ZV_MAX_H / ZV_BLK)
#define ZV_CB_N         256     // entries in each codebook
#define ZV_VEC          16      // 4x4 grey values an entry

// header flags
#define ZV_F_AUDIO      0x0001
#define ZV_F_COLOUR     0x0002  // a colour layer at colour_off (see below)

// audio formats
#define ZV_A_PCM8       1       // unsigned 8-bit, interleaved

// record kinds and flags
#define ZV_REC_FRAME    1
#define ZV_RF_KEY       0x01    // a seek point: all three codebooks sent whole; the
                                // picture is whole `refresh` frames later (header)

// block commands (base layer), in raster order of 8x8 blocks
#define ZV_CMD_SKIP     0x00    // 0x00-0x3f: n = (c & 63) + 1 blocks unchanged
#define ZV_CMD_V1       0x40    // 0x40-0x7f: n blocks, each one V1 index byte
#define ZV_CMD_V4       0x80    // 0x80-0xbf: n blocks, each four V4 index bytes
#define ZV_CMD_REP      0xc0    // 0xc0-0xdf: n = (c & 31) + 1 blocks, all one V1 index (one byte)
#define ZV_CMD_V16      0xe0    // 0xe0-0xff: n = (c & 31) + 1 blocks, each sixteen V16 index bytes
#define ZV_CMD_RUN(c)   (((c) & 63) + 1)
#define ZV_CMD_RUN32(c) (((c) & 31) + 1)
#define ZV_VEC16        4       // 2x2 grey values a V16 entry

typedef struct {
	uint16_t w, h;              // picture, multiples of 8, square pixels
	uint16_t flags;
	uint32_t fps_num, fps_den;  // exact source rate
	uint32_t frames;
	uint32_t arate;             // audio sample rate, Hz
	uint8_t achan;              // 1 or 2
	uint8_t afmt;               // ZV_A_PCM8
	uint32_t index_off;         // keyframe index: index_n x (u32 frame, u32 offset)
	uint32_t index_n;
	uint32_t colour_off;        // the colour layer (ZV_F_COLOUR), or 0
	uint32_t max_rec;           // largest record, header included
	uint32_t video_bytes;       // statistics, informational
	uint32_t audio_bytes;
	uint16_t refresh;           // frames after a seek point until the picture is whole (0: at once)
} zv_header_t;

// Parse the 64-byte file header. 0, or a negative error.
int zv_parse_header(const uint8_t *b, size_t n, zv_header_t *h);
// Write one (the encoder).
void zv_write_header(uint8_t *b, const zv_header_t *h);

// Audio sample frames before video frame `n`: the record for frame n
// carries samples [zv_audio_before(n), zv_audio_before(n + 1)).
uint32_t zv_audio_before(const zv_header_t *h, uint32_t n);

// Where to start decoding to show frame n: a frame at or before
// n - refresh (the player decodes from the seek point there, quietly,
// up to n). 0 for the first frames.
uint32_t zv_start_for(const zv_header_t *h, uint32_t n);

// The refresh (of a 60 Hz display) at which frame n is shown: the
// nearest one to its time. Any source rate works; 24 fps comes out as
// 2,3,2,3 refreshes.
uint32_t zv_frame_vsync(const zv_header_t *h, uint32_t n, uint32_t hz);

// ---- views ----------------------------------------------------------
//
// A view is what the decoder draws. Every one comes from the same base
// layer; the player picks the one its screen wants.
//
//   ZV_VIEW_GREY_FULL   8 bits a pixel, w x h, undithered (reference,
//                       tests, the encoder's own reconstruction)
//   ZV_VIEW_GREY_HALF   8 bits, w/2 x h/2, undithered (reference)
//   ZV_VIEW_MONO_FULL   1 bit, w x h: the desktop's full screen
//   ZV_VIEW_MONO_HALF   1 bit, w/2 x h/2: the window, game mode
//   ZV_VIEW_PAL_HALF    8 bits, w/2 x h/2, indices into a grey palette
//                       (zv_grey_palette()), dithered: chunky game mode
//   ZV_VIEW_PAL_FULL    the same at w x h: chunky game mode for a film of
//                       320x240 or less (archive.org's small copies)
//
// 1-bit views are in the framebuffer's order: bit 0 of each byte is its
// leftmost pixel, bytes in address order (little-endian words then put
// the leftmost pixel in bit 0, as z_fb_set_pixel() does).

enum {
	ZV_VIEW_GREY_FULL = 0,
	ZV_VIEW_GREY_HALF,
	ZV_VIEW_MONO_FULL,
	ZV_VIEW_MONO_HALF,
	ZV_VIEW_PAL_HALF,
	ZV_VIEW_PAL_FULL,
	// The palette views' 16 greys as four bit planes, each laid out as a
	// mono view's picture (ZV_VIEW_MONO_FULL's, _HALF's): plane k is
	// plane_off * k bytes after the first, and a pixel's grey is its
	// four bits, plane 0 the lowest -- the index ZV_VIEW_PAL_* would
	// give. For a display made of bit planes (the colour window,
	// docs/color.md): no chunky-to-planar pass. ZV_GREYS_TRUE only.
	ZV_VIEW_PL_FULL,
	ZV_VIEW_PL_HALF,
	ZV_VIEW_COUNT
};

enum {
	ZV_MASK_BAYER8 = 0,         // ordered: the classic crosshatch
	ZV_MASK_VAC8,               // void-and-cluster tile: finer, less regular
	ZV_MASK_COUNT
};

#define ZV_PAL_MAX 64           // grey palette entries at most

typedef struct {
	zv_header_t h;
	int bw, bh;                 // blocks across, down
	int view, mask;
	uint32_t align_;            // the books below are word aligned
	uint8_t cb1[ZV_CB_N][ZV_VEC];
	uint8_t cb4[ZV_CB_N][ZV_VEC];
	uint8_t cb16[ZV_CB_N][ZV_VEC16];
	// each entry drawn for the view the first time a block uses it at a
	// given place in the 8x8 mask (ok bits), and again after it changes
	uint32_t r1[ZV_CB_N][16];   // 64 bytes each, word aligned
	uint32_t r4[ZV_CB_N][32];   // (128 bytes: the half plane view's)
	uint32_t r16[ZV_CB_N][16];
	uint16_t ok1[ZV_CB_N], ok4[ZV_CB_N], ok16[ZV_CB_N];
	// brightness: gamma-encoded grey -> linear 0..4096
	uint16_t lin[256];
	// a flat field of each grey, dithered: 8 rows of the 8x8 mask, bit x
	// of row y white at (x, y)
	uint8_t pat[256][8];
	// the plane views': the same for each of the four planes, plane k in
	// byte k -- bit x of that byte is bit k of the grey's palette index
	// at (x, y)
	uint32_t pat4[256][8];
	int plane_off;              // the plane views: bytes from a plane to the next
	// the palette view: grey -> lower palette index + fraction (0..64)
	uint8_t pal_lo[256], pal_frac[256];
	int pal_n;
	int greys;                  // ZV_GREYS_*
	int drawn;                  // the books have been drawn for this view
	uint8_t *dirty;             // if set: bw x bh bytes, a block drawn sets its byte to 1
	const uint32_t *colw;       // the palette views: if set, the colour layer's
	                            // classes as zv_col_words() keeps them, ORed in
	                            // as each block is drawn
	                            // (the caller clears them): which blocks a frame changed
	int generic;                // tests: the plain per-block code, not the board's loops
	uint32_t frame;             // the next frame expected
	int have_key;               // a keyframe has been seen since init/seek
} zv_dec_t;

// Set up for a file and a view. `gamma` x100 is how the grey is turned
// into light before the mono views dither it: 0 for the default, 1.6.
// (2.2 is the display's own and keeps tones exact seen from afar; 1.6
// keeps more of the shadows at a desk. The palette view always uses 2.2.)
#define ZV_GAMMA_DEFAULT 160
void zv_dec_init(zv_dec_t *d, const zv_header_t *h, int view, int mask, int gamma100);
// Change the view or the mask (redraws every entry).
void zv_dec_set_view(zv_dec_t *d, int view, int mask);
// Which grey palette the palette view uses (ZV_GREYS_*; redraws).
void zv_dec_set_greys(zv_dec_t *d, int kind);
// After a seek: drop everything until the next keyframe.
void zv_dec_seek(zv_dec_t *d, uint32_t frame);

// A picture from a mono view into the plane view's layout and back, for
// changing between them mid-film with no seek (the decoder's books do
// not depend on the view: zv_dec_set_view(), then carry on). `words` is
// a row's 32-pixel words, `h` the rows; strides in bytes, multiples of 4.
// To planes: white is grey 15, black 0 -- the dithered picture as it
// was, which turns to greys as blocks are drawn. To mono: the 16 greys
// dithered with the 8x8 mask `mask` (ZV_MASK_*).
void zv_mono_to_planes(const uint32_t *mono, int mono_st, uint32_t *pl, int pl_st, int plane_off, int words, int h);
void zv_planes_to_mono(const uint32_t *pl, int pl_st, int plane_off, uint32_t *mono, int mono_st, int words, int h, int mask);

// The output picture of the current view: its size in pixels and bytes
// a row (the minimum; a caller may use a larger stride).
void zv_view_size(const zv_dec_t *d, int *w, int *h, int *min_stride);

// The video part of a record (after the record header and the audio).
// `out` is the view's picture, kept by the caller from frame to frame:
// unchanged blocks are not drawn. 0, or a negative error.
int zv_dec_video(zv_dec_t *d, const uint8_t *p, size_t n, int key,
                 uint8_t *out, int stride);

// Split a record: its kind, flags, audio and video. 0 or negative.
typedef struct {
	int kind, flags;
	uint32_t n_audio;           // sample frames
	const uint8_t *audio;       // n_audio x achan bytes
	const uint8_t *video;
	size_t video_len;
} zv_rec_t;
int zv_split_record(const zv_header_t *h, const uint8_t *r, size_t n, zv_rec_t *out);

// The grey palette for ZV_VIEW_PAL_HALF: up to `max` 12-bit colours
// (0xRGB), dark to light, black and white included. The 12-bit palette
// has only 16 true greys:
//   ZV_GREYS_NEAR  up to 64: near-greys (channels at most one step
//                  apart) add the levels between, each with a faint tint
//   ZV_GREYS_TRUE  the 16 true greys, dithered between: the default (the
//                  near-greys' tints show as pink and green in flat areas)
// Returns the count.
enum { ZV_GREYS_NEAR = 0, ZV_GREYS_TRUE = 1 };
int zv_grey_palette(uint16_t *pal12, int max, int kind);

// Linear brightness (0..4096) of a 12-bit colour, for the palette view
// and for tests.
uint16_t zv_lin12(uint16_t rgb12, int gamma100);

// ---- the colour layer -------------------------------------------------------
//
// Optional, after the index, so mono players never read it. It colours
// the palette views: each cell -- an 8x8 block, or a quarter of one --
// gets one of 16 chroma classes, and a pixel's palette index is
// class * 16 + its grey level
// (0..15, ZV_GREYS_TRUE). Class 0 is always neutral, so indices 0..15 are
// the 16 greys and a black and white film needs no layer at all.
//
//   0   "ZVC1"
//   4   u16 cw, ch            cells across and down (w/cell, h/cell)
//   8   u32 groups
//   12  u8 cell                 8, or 4 (finer, four times the bytes: for
//                               films shown at full size); 0 means 8
//   13  3 bytes 0
//   16  groups x (u32 first frame, u32 offset from colour_off)
//
// A group is one seek point's frames (first frame to the next group's):
//   16 x (s8 u, s8 v)       the classes' chroma (BT.601, full range,
//                           -128..127); class 0 is (0, 0)
//   then a frame each:      u16 n, n bytes of cell runs in raster order:
//     0x00-0x7f             (c + 1) cells unchanged
//     0x80-0xff             (c & 127) + 1 cells, two a byte (low nibble first)
// A group's first frame sets every cell.
#define ZV_COL_MAGIC    0x3143565au   // "ZVC1"
#define ZV_COL_CLASSES  16
#define ZV_COL_HDR      16
#define ZV_COL_TABLE    32            // a group's chroma table

typedef struct {
	uint16_t cw, ch;
	uint32_t groups;
	uint8_t cell;               // 8 or 4
} zv_col_header_t;

// The layer's header (the first 16 bytes); 0 or negative.
int zv_col_parse_header(const uint8_t *p, size_t n, zv_col_header_t *c);

// One frame's cell runs into cls[cw * ch]; returns the bytes used (the
// u16 included), or negative. With `mark`, each cell whose class changed
// gets mark[i] = 1 (others are left alone).
int zv_col_frame(const uint8_t *p, size_t n, uint8_t *cls, int cells, uint8_t *mark);

// The classes into the blocks of a palette view's picture that `map`
// marks (a byte an 8x8 block, as zv_dec_t.dirty): `bs` the block's size
// there (4 half, 8 full), `cell` the layer's (8: a class a block; 4:
// four), `out` and `stride` 4-aligned. The board's way: only what
// changed, a word at a time.
void zv_col_apply_map(const uint8_t *cls, const uint8_t *map, int bw, int bh, int bs, int cell,
                      uint8_t *out, int stride);

// zv_col_frame() for a player keeping the classes in its decoder
// (zv_dec_t.colw): each 8x8 block with a cell whose class changed is
// listed once -- list[] gets by << 8 | bx, *nlist the count, bmark (a
// byte a block, zero at the start) marks what is listed, for the caller
// to clear -- so that only those blocks need zv_col_words() and, where
// the frame's decode does not draw them, zv_col_apply_list(). A frame
// changes a few dozen cells, and a scan of every block would cost more
// than all of them. Returns the bytes used, or negative.
int zv_col_frame_blocks(const uint8_t *p, size_t n, uint8_t *cls, int cw, int ch, int cell,
                        uint8_t *bmark, uint16_t *list, int *nlist);

// The classes as the decoder takes them (zv_dec_t.colw): four words a
// block, its quarters top left, top right, bottom left, bottom right,
// each the class << 4 in all four bytes -- no multiply in the decoder's
// loops (picorv32's is 32 cycles). Only the blocks `bmark` marks (all,
// if NULL) are worked out again.
void zv_col_words(const uint8_t *cls, int cw, int ch, int cell, const uint8_t *bmark, uint32_t *colw);

// ...for the listed blocks only (zv_col_frame_blocks()'s list)
void zv_col_words_list(const uint8_t *cls, int cw, int cell, const uint16_t *list, int n, uint32_t *colw);

// The listed blocks' classes (as zv_col_words() keeps them) into a
// palette view's picture: `bs` the block's size there, 4 or 8.
void zv_col_apply_list(const uint32_t *colw, int bw, const uint16_t *list, int n, int bs,
                       uint8_t *out, int stride);

// The 256-colour palette (12-bit 0xRGB) for a group's chroma table:
// entry class * 16 + level, level 0..15 the true greys' brightness.
void zv_col_palette(const int8_t *uv, uint16_t *pal12);

// Put the classes into a palette view's picture (the grey levels in the
// low nibble): `px` is a cell's size in the picture (the layer's cell
// for ZV_VIEW_PAL_FULL, half of it for ZV_VIEW_PAL_HALF).
void zv_col_apply(const uint8_t *cls, int cw, int ch, int px, uint8_t *out, int stride);

#endif
