/*
 * zfpga -- FPGA bitstreams built on Zeitlos
 *
 * One header for the whole tool. See docs/zfpga.md.
 */

#ifndef ZFPGA_H
#define ZFPGA_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#include "zfpga_port.h"

#define ZFPGA_VERSION "0.1"

/* ---------------------------------------------------------------------
 * util.c -- arena, formatting, diagnostics
 *
 * The arena never frees. zfpga runs once, produces one output and exits,
 * so every allocation lives until then -- the same reasoning, and the
 * same allocator shape, as zcc's zalloc (docs/zcc.md, "zalloc was
 * quadratic"). One allocation per token through a first-fit malloc is
 * what cost zcc 70x on the device.
 */

void *zf_alloc(size_t n);               /* zeroed, 4-byte aligned; never NULL */
void *zf_alloc_raw(size_t n);           /* not zeroed; for buffers filled whole */
char *zf_strdup(const char *s);
int zf_memcmp(const void *a, const void *b, size_t n);

/* 8.3 names (the card's FatFs has no long names): whether a path fits,
 * and the tail of an error message saying so where it matters. */
int zf_is_83(const char *path);
const char *zf_83_hint(const char *path);

/* Hand back everything allocated since a mark. For `zfpga build`,
 * between stages; nothing allocated after the mark may be used after. */
typedef struct { void *blocks; uint8_t *p; size_t left; } zf_mark_t;
zf_mark_t zf_mark(void);
void zf_release(zf_mark_t m);
size_t zf_mem_peak(void);
void zdb_forget_if(const void *block);     /* db.c: zf_release() tells it what it frees */

/* A tiny formatter: %s %d %u %x %X %c %%, with an optional width, '0'
 * flag on the integer conversions and '-' (left-justify) on %s. Not printf: newlib's costs about
 * 100KB on the device (docs/app_runtime.md), and a tool made of
 * diagnostics would pay it on every build. */
int zf_vfmt(char *out, int outlen, const char *fmt, va_list ap);
int zf_fmt(char *out, int outlen, const char *fmt, ...);

void zf_print(const char *fmt, ...);            /* stdout */
void zf_note(const char *fmt, ...);             /* "zfpga: ..." */

/* file:line: error: message, then exit(1). There is no error recovery:
 * the first true message beats twenty plausible ones. */
void zf_fatal_at(const char *file, int line, const char *fmt, ...) __attribute__((noreturn));
void zf_fatal(const char *fmt, ...) __attribute__((noreturn));

int zf_streq(const char *a, const char *b);
int zf_strcmp(const char *a, const char *b);
size_t zf_strlen(const char *s);
void zf_memset(void *p, int c, size_t n);
void zf_memcpy(void *d, const void *s, size_t n);
int zf_parse_uint(const char *s, uint32_t *out);  /* dec, or 0x hex */

/* ---------------------------------------------------------------------
 * Buffered reading and writing over the port layer.
 *
 * Input is read in chunks and handed back a line at a time, so a
 * multi-megabyte .config never has to be resident: the SOC's own
 * configuration is streamed. Output is written in chunks for the same
 * reason. docs/posix.md sec. 2.5 explains why whole-file reads are the
 * wrong shape on this machine: FatFs is not preempted inside a call.
 */

#define ZF_LINE_MAX 2048

typedef struct {
    zio_file_t *f;
    const char *path;
    char buf[4096];
    int len, pos;
    int eof;
    int lineno;
    char line[ZF_LINE_MAX];
} zf_reader_t;

void zf_reader_open(zf_reader_t *r, const char *path);
/* Returns the next line with trailing CR/LF removed, or NULL at EOF. */
char *zf_reader_line(zf_reader_t *r);
void zf_reader_close(zf_reader_t *r);

/* Read a whole file into the arena. For the database only, which is
 * random-access and so has to be resident. */
uint8_t *zf_read_all(const char *path, uint32_t *len);

typedef struct {
    zio_file_t *f;
    const char *path;
    uint8_t buf[4096];
    int len;
    uint32_t total;
} zf_writer_t;

void zf_writer_open(zf_writer_t *w, const char *path);
void zf_writer_byte(zf_writer_t *w, uint8_t b);
void zf_writer_bytes(zf_writer_t *w, const uint8_t *p, uint32_t n);
void zf_writer_close(zf_writer_t *w);

/* Split a line into whitespace-separated tokens, in place. '#' starts
 * a comment. Returns the token count; at most `max`. */
int zf_tokens(char *line, char **tok, int max);

/* ---------------------------------------------------------------------
 * db.c -- the packed chip database (.zdb)
 *
 * Produced by tools/mkzdb.py from the vendored prjtrellis-db. Loaded
 * whole and used in place: every record refers to others by index and
 * to strings by offset, so there are no pointers to fix up.
 *
 * All integers little-endian. RV32 and x86 both are; the header's magic
 * would read backwards on anything else and the load would refuse.
 *
 * Layout: a 128-byte header, then the sections in ZDB_S_* order, each
 * 4-byte aligned. Strings are NUL-terminated; offset 0 is "".
 *
 * Every name list (tiles, a type's muxes, a mux's arcs, words, enums, an
 * enum's options) is sorted bytewise, which is both std::map order --
 * the order libtrellis applies defaults in -- and strcmp order, so
 * lookups are binary searches.
 *
 * A bit is a u16: frame << 4 | bit, with bit 15 set if inverted. Tiles
 * are at most 106 frames by 12 bits on every ECP5 die, which is why
 * that fits. Tiles are also DISJOINT in CRAM on every ECP5 die --
 * checked across 25F, 45F and 85F while this was designed -- so the
 * order tiles are configured in cannot affect the result. That is what
 * lets a .config be streamed rather than held.
 */

#define ZDB_VERSION 3
#define ZDB_HDR_SIZE 256
#define ZDB_FAMILY_ECP5 1

enum {
    ZDB_S_STR, ZDB_S_VAR, ZDB_S_TILE, ZDB_S_TYPE, ZDB_S_MUX, ZDB_S_ARC,
    ZDB_S_WORD, ZDB_S_GRP, ZDB_S_ENUM, ZDB_S_OPT, ZDB_S_BIT,
    ZDB_S_PIO, ZDB_S_PIN, ZDB_S_BASE, ZDB_S_FIX,
    ZDB_NSECT
};

typedef struct { uint32_t off, count; } zdb_sect_t;

typedef struct {
    char magic[4];                  /* "ZDB1" */
    uint32_t version;
    uint32_t size;
    uint32_t family;
    uint16_t frames;
    uint16_t bits_per_frame;
    uint8_t pad_before;
    uint8_t pad_after;
    uint8_t n_variants;
    uint8_t reserved0;
    uint32_t source;                /* string: upstream commit */
    uint32_t reserved1;
    zdb_sect_t sect[ZDB_NSECT];
    uint8_t pad[ZDB_HDR_SIZE - 32 - 8 * ZDB_NSECT];
} zdb_hdr_t;

typedef struct { uint32_t name, idcode; } zdb_var_t;

typedef struct {
    uint32_t name;
    uint16_t type;
    uint16_t start_frame;
    uint16_t start_bit;
    uint8_t n_frames;
    uint8_t n_bits;
} zdb_tile_t;

typedef struct {
    uint32_t name;
    uint32_t mux0, n_mux;
    uint32_t word0, n_word;
    uint32_t enum0, n_enum;
    uint32_t fix0, n_fix;   /* v3: fixed connections, sorted by source */
} zdb_type_t;

typedef struct { uint32_t sink, source; } zdb_fix_t;

typedef struct { uint32_t sink, arc0, n_arc; } zdb_mux_t;
typedef struct { uint32_t source, bit0, n_bit; } zdb_arc_t;
typedef struct { uint32_t name, defval, grp0, n_grp; } zdb_word_t;
typedef struct { uint32_t bit0, n_bit; } zdb_grp_t;
typedef struct { uint32_t name, opt0, n_opt; int32_t defopt; } zdb_enum_t;
typedef struct { uint32_t name, bit0, n_bit; } zdb_opt_t;

/* Version 2: one record per PIO site, with the tiles `zfpga pnr` writes
 * for it. The side rules are nextpnr-ecp5's get_pio_tile() and
 * get_pic_tile(); the tristate tie is the CIB mux feeding JPADDT<L>,
 * resolved from the PIC tile's fixed connections. mkzdb.py computes
 * all of it at build time. tie_tile is -1 if there is none. */
typedef struct {
    uint16_t row, col;
    uint8_t letter;         /* 'A'..'D' */
    uint8_t bank;
    uint16_t reserved;
    int32_t pio_tile, pic_tile, tie_tile;
    uint32_t tie_enum;      /* string: "CIB.JB0MUX" etc. */
} zdb_pio_t;

/* Package pin name -> PIO, for every package the die comes in. */
typedef struct { uint32_t package, pin, pio; } zdb_pin_t;

#define ZDB_BIT_FRAME(b) (((b) >> 4) & 0x7ff)
#define ZDB_BIT_BIT(b)   ((b) & 0xf)
#define ZDB_BIT_INV(b)   (((b) >> 15) & 1)

typedef struct {
    const uint8_t *base;
    uint32_t size;
    const zdb_hdr_t *hdr;
    const char *str;
    const zdb_var_t *var;
    const zdb_tile_t *tile;
    const zdb_type_t *type;
    const zdb_mux_t *mux;
    const zdb_arc_t *arc;
    const zdb_word_t *word;
    const zdb_grp_t *grp;
    const zdb_enum_t *en;
    const zdb_opt_t *opt;
    const uint16_t *bit;
    const zdb_pio_t *pio;
    const zdb_pin_t *pin;
    const zdb_fix_t *fix;
    const char *baseline;   /* baseline .config text, not NUL-terminated */
    uint32_t n_tile, n_pio, n_pin, baseline_len;

    /* The device this run is for, which may be a variant (12F) served
     * by another die's database (25F). */
    const char *device;
    uint32_t idcode;
} zdb_t;

/* Find and load the database for a device name from `.device`. */
void zdb_load(zdb_t *db, const char *dir, const char *device);

#define ZDB_STR(db, off) ((db)->str + (off))

int32_t zdb_find_tile(const zdb_t *db, const char *name);
int32_t zdb_find_tile_by_type(const zdb_t *db, const char *type, int *count);
const zdb_mux_t *zdb_find_mux(const zdb_t *db, const zdb_type_t *t, const char *sink);
const zdb_arc_t *zdb_find_arc(const zdb_t *db, const zdb_mux_t *m, const char *source);
const zdb_word_t *zdb_find_word(const zdb_t *db, const zdb_type_t *t, const char *name);
const zdb_enum_t *zdb_find_enum(const zdb_t *db, const zdb_type_t *t, const char *name);
int32_t zdb_find_opt(const zdb_t *db, const zdb_enum_t *e, const char *name);

/* Maps a device name to the database file that serves it, e.g.
 * LFE5U-12F -> lfe5u25f. The .zdb then confirms it by listing the
 * device among its variants. */
const char *zdb_file_for(const char *device);

/* ---------------------------------------------------------------------
 * chip.c -- configuration memory, and applying a config to it
 */

typedef struct {
    zdb_t *db;
    uint32_t frames, bpf, bytes_per_frame, pad_after;
    uint8_t *cram;          /* frames * bytes_per_frame, in OUTPUT layout */
    uint8_t *seen;          /* one flag per tile: configured explicitly */
    uint32_t usercode;
    uint32_t ctrl0;

    /* .comment lines, in order: the bitstream's metadata header */
    char **meta;
    int n_meta, max_meta;

    /* .sysconfig key/value pairs that ecppack honours */
    const char *mcclk_freq;
    int compress_config;

    /* .bram_init, pre-packed into 72-bit frames exactly as written */
    struct zf_bram *bram;
} zf_chip_t;

struct zf_bram {
    struct zf_bram *next;
    uint32_t index;
    uint8_t data[256 * 9];  /* 2048 x 9-bit words -> 256 frames of 9 bytes */
};

void chip_init(zf_chip_t *c, zdb_t *db);
void chip_set_bit(zf_chip_t *c, uint32_t frame, uint32_t bit, int v);
int chip_get_bit(const zf_chip_t *c, uint32_t frame, uint32_t bit);

/* One entry of a .tile or .tile_group record. Names and values point
 * into the record's own string pool, valid until the record is applied. */
enum { ZF_T_ARC, ZF_T_WORD, ZF_T_ENUM, ZF_T_UNKNOWN };

typedef struct {
    uint8_t kind;
    const char *name;       /* arc: sink */
    const char *value;      /* arc: source */
    uint32_t f, b;          /* unknown: tile-relative bit */
    int line;
} zf_tent_t;

/* Applies one tile's entries in libtrellis's order, then its defaults.
 * With `tg` set (a .tile_group), missing names are skipped rather than
 * refused, no defaults are applied, and `matched[i]` records which
 * entries found a home. */
void chip_apply_tile(zf_chip_t *c, uint32_t tix, const zf_tent_t *e, int n,
    const char *file, int tg, uint8_t *matched);

/* Defaults for every tile no .tile record mentioned. */
void chip_default_unseen(zf_chip_t *c);

/* cfg.c: read a .config, streaming. Loads the database named by its
 * .device line from `dbdir` into `db`, and initialises `c`. */
void cfg_load(zf_chip_t *c, zdb_t *db, const char *dbdir, const char *path);

/* ---------------------------------------------------------------------
 * pack.c -- serialisation
 */

typedef struct {
    int compress;
    const char *freq;       /* "2.4", "38.8", ... or NULL */
    const char *spimode;    /* "fast-read", "dual-spi", "qspi" or NULL */
    int multiboot;          /* -1 unset, 0 no, 1 yes */
    int background;
    int have_usercode;
    uint32_t usercode;
    int have_idcode;
    uint32_t idcode;
    int have_bootaddr;
    uint32_t bootaddr;
} zf_packopts_t;

void pack_apply_options(zf_chip_t *c, zf_packopts_t *o);
void pack_write(zf_chip_t *c, const zf_packopts_t *o, const char *out_path);

/* ---------------------------------------------------------------------
 * Subcommands
 */

int cmd_pack(int argc, char **argv);
int cmd_info(int argc, char **argv);
int cmd_pnr(int argc, char **argv, const char *dbdir_default);
int cmd_place(int argc, char **argv, const char *dbdir_default);
int cmd_unpack(int argc, char **argv, const char *dbdir_default);
int cmd_synth(int argc, char **argv);
int cmd_build(int argc, char **argv, const char *dbdir_default);
int cmd_bram(int argc, char **argv, const char *dbdir_default);
extern char unpack_pack_opts[160];      /* unpack.c: the last unpack's pack options */

/* ---------------------------------------------------------------------
 * route.c -- the relative-name resolver and the router (Phase 4)
 */

typedef struct {
    int16_t row, col;
    uint32_t name;          /* string-table offset of the base name */
} gwire_t;

typedef void (*arc_cb)(int net, uint32_t tile, const char *sink, const char *source);

typedef struct {
    gwire_t src;
    gwire_t *sinks;
    int n_sinks;
} rnet_t;

typedef struct {
    uint32_t iterations, reroutes, expanded, overused, arcs, wires;
} route_stats_t;

void route_init(zdb_t *d);
int resolve(int row, int col, const char *name, gwire_t *out);
int route_parse_wire(const char *s, gwire_t *w);        /* "R2C5/MUXCLK0" */
int route_all(const rnet_t *nets, int n_nets, arc_cb emit, route_stats_t *st, int *sink);
const char *route_wire_str(const gwire_t *w);

#endif
