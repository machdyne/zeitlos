/*
 * zfpga -- turning configuration memory into an ECP5 bitstream.
 *
 * This is ecppack (Project Trellis 1.4) re-expressed, and it has to
 * produce the same bytes: the test in tests/run.sh is byte identity with
 * ecppack on the same .config, including the real Zeitlos SOC.
 *
 * Format summary (https://prjtrellis.readthedocs.io/, "Bitstream
 * format"; Lattice FPGA-TN-02039):
 *
 *   header      FF 00, the .comment strings each NUL-terminated, FF
 *   preamble    FF FF BD B3, then four dummy FF
 *   commands    8-bit opcode, 24 bits of operand, payload, optional CRC
 *   frames      highest-numbered first, each followed by a CRC16 and
 *               one FF; plain (LSC_PROG_INCR_RTI) or compressed
 *               (LSC_PROG_INCR_CMP)
 *   EBR         LSC_EBR_ADDRESS + LSC_EBR_WRITE per block RAM
 *   end         ISC_PROGRAM_DONE and four dummy FF
 *
 * The CRC is polynomial 0x8005, no reflection, initial 0 -- "CRC-16/
 * BUYPASS". It runs over every byte written with wb(), is emitted by
 * crc_out() and restarts there, and does not see dummy() bytes. Where
 * the result is surprising, the reference is the surprise: the FF after
 * each frame IS counted, and lands in the next frame's CRC, despite the
 * format notes saying dummy bytes between frames are excluded.
 */

#include "zfpga.h"

enum {
    CMD_SPI_MODE = 0x79,
    CMD_LSC_RESET_CRC = 0x3B,
    CMD_VERIFY_ID = 0xE2,
    CMD_LSC_WRITE_COMP_DIC = 0x02,
    CMD_LSC_PROG_CNTRL0 = 0x22,
    CMD_LSC_INIT_ADDRESS = 0x46,
    CMD_LSC_PROG_INCR_CMP = 0xB8,
    CMD_LSC_PROG_INCR_RTI = 0x82,
    CMD_ISC_PROGRAM_USERCODE = 0xC2,
    CMD_LSC_EBR_ADDRESS = 0xF6,
    CMD_LSC_EBR_WRITE = 0xB2,
    CMD_ISC_PROGRAM_DONE = 0x5E,
};

/* ECP5 values from libtrellis's BitstreamOptions: these mimic Diamond. */
#define ECP5_DUMMY_AFTER_PREAMBLE 4
#define ECP5_CRC_META             0x91  /* CRC on, per frame, 1 dummy byte */
#define ECP5_DUMMY_AFTER_FRAME    1
#define ECP5_SECURITY_SED_SPACE   12

#define MULTIBOOT_FLAG  (1u << 20)
#define BACKGROUND_FLAG 0x2E000000u

static const struct { const char *name; uint8_t code; } freqs[] = {
    { "2.4", 0x00 }, { "4.8", 0x01 }, { "9.7", 0x20 },
    { "19.4", 0x30 }, { "38.8", 0x38 }, { "62.0", 0x3b },
};

static const struct { const char *name; uint8_t code; } spimodes[] = {
    { "fast-read", 0x49 }, { "dual-spi", 0x51 }, { "qspi", 0x59 },
};

/* -- CRC ---------------------------------------------------------------
 * Table-driven. libtrellis shifts message bits in at the bottom and
 * flushes 16 zero bits before emitting ("augmented"); the direct table
 * form below computes the identical value without the flush. */

static uint16_t crc_tab[256];

static void crc_init_table(void) {
    int i, k;
    for (i = 0; i < 256; i++) {
        uint16_t c = (uint16_t)(i << 8);
        for (k = 0; k < 8; k++)
            c = (uint16_t)((c & 0x8000) ? (c << 1) ^ 0x8005 : (c << 1));
        crc_tab[i] = c;
    }
}

typedef struct {
    zf_writer_t w;
    uint16_t crc;
    /* A dry pass writes nothing and only counts: a jumploader's header
     * describes offsets within the stream that follows it (see -J). */
    int dry;
    uint32_t pos;           /* bytes since the preamble's first FF */
    uint32_t crc_start;     /* where the running CRC last restarted */
} bs_t;

static void wb(bs_t *s, uint8_t b) {
    if (!s->dry) zf_writer_byte(&s->w, b);
    s->pos++;
    s->crc = (uint16_t)((s->crc << 8) ^ crc_tab[((s->crc >> 8) ^ b) & 0xff]);
}

/* A run of bytes: CRC over them, then one copy into the writer. The
 * uncompressed frames are 560KB of this. */
static void wbytes(bs_t *s, const uint8_t *p, uint32_t n) {
    uint16_t crc = s->crc;
    uint32_t i;
    for (i = 0; i < n; i++)
        crc = (uint16_t)((crc << 8) ^ crc_tab[((crc >> 8) ^ p[i]) & 0xff]);
    s->crc = crc;
    if (!s->dry) zf_writer_bytes(&s->w, p, n);
    s->pos += n;
}

static void dummy(bs_t *s, int n) {
    while (n--) {
        if (!s->dry) zf_writer_byte(&s->w, 0xFF);
        s->pos++;
    }
}

static void zeros(bs_t *s, int n) {
    while (n--) wb(s, 0);
}

static void u32(bs_t *s, uint32_t v) {
    wb(s, (uint8_t)(v >> 24));
    wb(s, (uint8_t)(v >> 16));
    wb(s, (uint8_t)(v >> 8));
    wb(s, (uint8_t)v);
}

static void crc_out(bs_t *s) {
    uint16_t c = s->crc;
    wb(s, (uint8_t)(c >> 8));
    wb(s, (uint8_t)c);
    s->crc = 0;
    s->crc_start = s->pos;
}

/* -- jumploaders (-J) --------------------------------------------------
 * A jumploader's boot address can be changed IN FLASH, by the machine,
 * without re-packing: docs/zboot.md sec. 5. In a compressed bitstream a
 * frame's length depends on its bytes, so changing the address would
 * move everything after it. With -J, the frames holding BOOTADDR's
 * eight bits are encoded with the literal code only -- 10 bits a byte,
 * whatever the byte -- and are left out of the dictionary's histogram,
 * so neither their length nor the dictionary depends on the address.
 * Each frame is byte-aligned and carries its own CRC, so changing one
 * changes nothing outside it but its CRC.
 *
 * The header then says where the eight bits are and which CRCs cover
 * them (pack_write), as offsets from the preamble's first FF -- so the
 * header's own length does not matter:
 *
 *   ZJUMP1 b=OOOOOO.MM,... (bit 0 first) c=FFFFFF.AAAAAA,...
 *
 * b: the stream byte and mask of each address bit, in the literal code
 * where the bit sits as itself; c: for each patched frame, the first
 * byte its CRC covers and the byte where the CRC (2 bytes, MSB first)
 * is. CRC-16/BUYPASS, as below. sw/common/zjump.c reads it. */

#define JUMP_BITS 8
#define JUMP_MAX_FRAMES 8

static struct {
    int on;
    uint32_t frame[JUMP_BITS], byte[JUMP_BITS];     /* where each address bit is in CRAM */
    uint8_t mask[JUMP_BITS];
    uint32_t pframe[JUMP_MAX_FRAMES];               /* the frames holding them */
    int n_pframe;
    uint32_t b_off[JUMP_BITS];                      /* ...and in the stream */
    uint8_t b_mask[JUMP_BITS];
    uint32_t c_from[JUMP_MAX_FRAMES], c_at[JUMP_MAX_FRAMES];
    int n_c, n_b;
} J;

static int jump_frame(uint32_t f) {
    int k;
    for (k = 0; k < J.n_pframe; k++) if (J.pframe[k] == f) return 1;
    return 0;
}

/* -- options -----------------------------------------------------------
 * ecppack's main(), in its order: usercode and idcode, then the options
 * nextpnr passes through .sysconfig, then the command line on top, then
 * the two CRAM edits (background reconfiguration, boot address). */

static const zdb_tile_t *only_tile_of_type(zf_chip_t *c, const char *type) {
    int n;
    int32_t t = zdb_find_tile_by_type(c->db, type, &n);
    if (t < 0 || n != 1)
        zf_fatal("expected exactly one %s tile, found %d", type, n);
    return &c->db->tile[t];
}

void pack_apply_options(zf_chip_t *c, zf_packopts_t *o) {
    if (o->have_usercode) c->usercode = o->usercode;
    if (o->have_idcode) c->db->idcode = o->idcode;

    if (!o->freq && c->mcclk_freq)
        o->freq = zf_streq(c->mcclk_freq, "62") ? "62.0" : c->mcclk_freq;
    if (c->compress_config) o->compress = 1;

    if (o->background) {
        const zdb_tile_t *t = only_tile_of_type(c, "EFB0_PICB0");
        const zdb_type_t *ty = &c->db->type[t->type];
        const zdb_enum_t *e = zdb_find_enum(c->db, ty, "SYSCONFIG.BACKGROUND_RECONFIG");
        int32_t on;
        uint32_t i;
        if (!e) zf_fatal("database has no SYSCONFIG.BACKGROUND_RECONFIG");
        on = zdb_find_opt(c->db, e, "ON");
        if (on < 0) zf_fatal("SYSCONFIG.BACKGROUND_RECONFIG has no ON");
        for (i = 0; i < c->db->opt[e->opt0 + (uint32_t)on].n_bit; i++) {
            uint16_t b = c->db->bit[c->db->opt[e->opt0 + (uint32_t)on].bit0 + i];
            chip_set_bit(c, t->start_frame + ZDB_BIT_FRAME(b),
                t->start_bit + ZDB_BIT_BIT(b), !ZDB_BIT_INV(b));
        }
    }

    /* BOOTADDR is addr[23:16], written into the one EFB1_PICB1 tile.
     * ecppack writes each bit to the VALUE and ignores the database's
     * inversion flag; so does this, because the point is to agree. See
     * docs/zboot.md sec. 2 for what the address does. */
    J.on = 0;
    if (o->jump && !o->have_bootaddr) zf_fatal("-J needs a boot address (-a)");
    if (o->jump && !o->compress) zf_fatal("-J is for compressed bitstreams (-c)");
    if (o->have_bootaddr) {
        const zdb_tile_t *t = only_tile_of_type(c, "EFB1_PICB1");
        const zdb_type_t *ty = &c->db->type[t->type];
        const zdb_word_t *w = zdb_find_word(c->db, ty, "BOOTADDR");
        uint32_t v, j, i;
        if (o->bootaddr & 0xffff)
            zf_fatal("boot address 0x%x is not 64K aligned", o->bootaddr);
        if (!w) zf_fatal("database has no BOOTADDR");
        v = (o->bootaddr & 0x00ff0000u) >> 16;
        J.on = o->jump;
        J.n_pframe = 0;
        if (o->jump && w->n_grp != JUMP_BITS)
            zf_fatal("-J: BOOTADDR has %u bits in this database, not %d", w->n_grp, JUMP_BITS);
        for (j = 0; j < w->n_grp; j++) {
            const zdb_grp_t *g = &c->db->grp[w->grp0 + j];
            if (o->jump && g->n_bit != 1)
                zf_fatal("-J: BOOTADDR bit %u is %u configuration bits, not 1", j, g->n_bit);
            for (i = 0; i < g->n_bit; i++) {
                uint16_t b = c->db->bit[g->bit0 + i];
                uint32_t fr = t->start_frame + ZDB_BIT_FRAME(b), bt = t->start_bit + ZDB_BIT_BIT(b);
                chip_set_bit(c, fr, bt, (v >> j) & 1);
                if (o->jump) {
                    uint8_t m;
                    uint32_t idx = chip_cram_index(c, fr, bt, &m);
                    J.frame[j] = idx / c->bytes_per_frame;
                    J.byte[j] = idx % c->bytes_per_frame;
                    J.mask[j] = m;
                    if (!jump_frame(J.frame[j])) {
                        if (J.n_pframe == JUMP_MAX_FRAMES) zf_fatal("-J: BOOTADDR spans too many frames");
                        J.pframe[J.n_pframe++] = J.frame[j];
                    }
                }
            }
        }
        o->multiboot = 1;
    }
}

/* -- compression -------------------------------------------------------
 * Four cases, per byte: 0 -> "0"; one bit set -> "100" + bit index;
 * one of eight dictionary bytes -> "101" + index; anything else ->
 * "11" + the byte. The dictionary is the eight most frequent bytes that
 * are neither zero nor one-hot, ties broken towards the larger byte
 * value -- which is what std::priority_queue<pair<int,uint8_t>> yields,
 * and so what this must yield. */

static int onehot(uint8_t b) {
    int i;
    for (i = 0; i < 8; i++)
        if (b == (uint8_t)(1u << i)) return i;
    return -1;
}

/* One bit at a time, MSB first, exactly as libtrellis's add_bit(). A
 * word accumulator was tried and was SLOWER on RV32 (21.7M vs 19.4M
 * instructions for a compressed blinky, under sim/): nearly every byte
 * is zero and costs one bit, so the simple form is already the fast
 * one -- provided it is inlined. always_inline because under -Os GCC
 * otherwise calls it, and a call per zero byte doubles the cost. */
typedef struct {
    bs_t *s;
    uint8_t buf;
    int n;
} bits_t;

static inline __attribute__((always_inline))
void bit_put(bits_t *b, int v) {
    if (v) b->buf |= (uint8_t)(1u << (7 - b->n));
    if (++b->n == 8) { wb(b->s, b->buf); b->buf = 0; b->n = 0; }
}

static void bits_put(bits_t *b, uint32_t x, int len) {
    int i;
    for (i = len - 1; i >= 0; i--) bit_put(b, (x >> i) & 1);
}

static void bits_flush(bits_t *b) {
    if (b->n) { wb(b->s, b->buf); b->buf = 0; b->n = 0; }
}

static void write_compressed(bs_t *s, zf_chip_t *c) {
    uint32_t hist[256];
    uint8_t dict[8];
    uint32_t f, i, B = c->bytes_per_frame;
    int k;
    bits_t bw;

    for (i = 0; i < 256; i++) hist[i] = 0;
    for (f = 0; f < c->frames; f++) {
        const uint8_t *fr = c->cram + f * B;
        if (J.on && jump_frame(f)) continue;    /* the dictionary must not depend on the address */
        for (i = 0; i < B; i++) hist[fr[i]]++;
    }

    for (k = 0; k < 8; k++) {
        int best = -1;
        for (i = 1; i < 256; i++) {
            int used = 0, j;
            if (onehot((uint8_t)i) >= 0) continue;
            for (j = 0; j < k; j++) if (dict[j] == i) used = 1;
            if (used) continue;
            if (best < 0 || hist[i] > hist[best] ||
                    (hist[i] == hist[best] && (int)i > best))
                best = (int)i;
        }
        dict[k] = (uint8_t)best;
    }

    wb(s, CMD_LSC_WRITE_COMP_DIC);
    zeros(s, 3);
    for (k = 7; k >= 0; k--) wb(s, dict[k]);

    wb(s, CMD_LSC_PROG_INCR_CMP);
    wb(s, ECP5_CRC_META);
    wb(s, (uint8_t)(c->frames >> 8));
    wb(s, (uint8_t)c->frames);

    bw.s = s; bw.buf = 0; bw.n = 0;
    J.n_b = J.n_c = 0;
    for (f = 0; f < c->frames; f++) {
        uint32_t fn = c->frames - 1 - f;
        const uint8_t *fr = c->cram + fn * B;
        if (B % 8)
            for (i = 0; i < 8 - B % 8; i++) bit_put(&bw, 0);
        if (J.on && jump_frame(fn)) {
            /* every byte literal, recording where each address bit lands */
            uint32_t from = s->crc_start;
            for (i = 0; i < B; i++) {
                int bit;
                bits_put(&bw, 3, 2);
                for (bit = 7; bit >= 0; bit--) {
                    int j;
                    for (j = 0; j < JUMP_BITS; j++)
                        if (J.frame[j] == fn && J.byte[j] == i && J.mask[j] == (uint8_t)(1u << bit)) {
                            J.b_off[j] = s->pos;
                            J.b_mask[j] = (uint8_t)(0x80u >> bw.n);
                            J.n_b++;
                        }
                    bit_put(&bw, (fr[i] >> bit) & 1);
                }
            }
            bits_flush(&bw);
            J.c_from[J.n_c] = from;
            J.c_at[J.n_c] = s->pos;
            J.n_c++;
            crc_out(s);
            for (k = 0; k < ECP5_DUMMY_AFTER_FRAME; k++) wb(s, 0xFF);
            continue;
        }
        for (i = 0; i < B; i++) {
            uint8_t b = fr[i];
            int oh;
            if (b == 0) { bit_put(&bw, 0); continue; }
            oh = onehot(b);
            if (oh >= 0) { bits_put(&bw, 4, 3); bits_put(&bw, (uint32_t)oh, 3); continue; }
            for (k = 0; k < 8; k++)
                if (dict[k] == b) break;
            if (k < 8) { bits_put(&bw, 5, 3); bits_put(&bw, (uint32_t)k, 3); continue; }
            bits_put(&bw, 3, 2);
            bits_put(&bw, b, 8);
        }
        bits_flush(&bw);
        crc_out(s);
        for (k = 0; k < ECP5_DUMMY_AFTER_FRAME; k++) wb(s, 0xFF);
    }
}

/* -- the whole bitstream ---------------------------------------------- */

static void pack_emit(bs_t *s, zf_chip_t *c, const zf_packopts_t *o, const char *extra_meta,
    const char *out_path);

void pack_write(zf_chip_t *c, const zf_packopts_t *o, const char *out_path) {
    bs_t *s = zf_alloc(sizeof(*s));
    char *desc = NULL;
    if (J.on) {
        /* the dry pass finds the offsets the header will describe */
        uint32_t n, k;
        int j;
        zf_memset(s, 0, sizeof(*s));
        s->dry = 1;
        pack_emit(s, c, o, NULL, NULL);
        if (J.n_b != JUMP_BITS) zf_fatal("-J: found %d of the address bits in the stream", J.n_b);
        desc = zf_alloc(16 + 10 * JUMP_BITS + 16 * (uint32_t)J.n_c);
        n = (uint32_t)zf_fmt(desc, 32, "ZJUMP1 b=");
        for (j = 0; j < JUMP_BITS; j++)
            n += (uint32_t)zf_fmt(desc + n, 16, "%s%06x.%02x", j ? "," : "", J.b_off[j], J.b_mask[j]);
        n += (uint32_t)zf_fmt(desc + n, 8, " c=");
        for (k = 0; k < (uint32_t)J.n_c; k++)
            n += (uint32_t)zf_fmt(desc + n, 20, "%s%06x.%06x", k ? "," : "", J.c_from[k], J.c_at[k]);
    }
    zf_memset(s, 0, sizeof(*s));
    pack_emit(s, c, o, desc, out_path);
    zf_writer_close(&s->w);
}

static void hbyte(bs_t *s, uint8_t b) {
    if (!s->dry) zf_writer_byte(&s->w, b);
}

static void hbytes(bs_t *s, const char *p) {
    if (!s->dry) zf_writer_bytes(&s->w, (const uint8_t *)p, (uint32_t)zf_strlen(p));
}

static void pack_emit(bs_t *s, zf_chip_t *c, const zf_packopts_t *o, const char *extra_meta,
    const char *out_path) {
    uint32_t ctrl0 = c->ctrl0, f, B = c->bytes_per_frame;
    int i;
    struct zf_bram *br;
    int spi = -1;

    crc_init_table();

    /* Everything that can be refused is refused before the output file
     * is opened, so a refusal never leaves a partial bitstream behind
     * -- which would otherwise program, or be flashed, as if whole. */
    if (o->spimode) {
        for (i = 0; i < (int)(sizeof(spimodes) / sizeof(spimodes[0])); i++)
            if (zf_streq(o->spimode, spimodes[i].name)) spi = i;
        if (spi < 0)
            zf_fatal("bad SPI mode %s (fast-read dual-spi qspi)", o->spimode);
    }

    if (o->freq) {
        for (i = 0; i < (int)(sizeof(freqs) / sizeof(freqs[0])); i++)
            if (zf_streq(o->freq, freqs[i].name)) break;
        if (i == (int)(sizeof(freqs) / sizeof(freqs[0])))
            zf_fatal("bad frequency %s (2.4 4.8 9.7 19.4 38.8 62.0)", o->freq);
        ctrl0 |= freqs[i].code;
    }
    if (o->multiboot == 1) ctrl0 |= MULTIBOOT_FLAG;
    else if (o->multiboot == 0) ctrl0 &= ~MULTIBOOT_FLAG;
    if (o->background) ctrl0 |= BACKGROUND_FLAG;

    /* Opened only now, once every option has been checked: a bad one
     * must not leave a partial output behind (tests/run.sh). */
    if (!s->dry) zf_writer_open(&s->w, out_path);

    /* Metadata header. Not part of the bitstream proper; the FPGA skips
     * it, vendor tools read it. A jumploader adds its ZJUMP1 line. */
    hbyte(s, 0xFF);
    hbyte(s, 0x00);
    for (i = 0; i < c->n_meta; i++) {
        hbytes(s, c->meta[i]);
        hbyte(s, 0x00);
    }
    if (extra_meta) {
        hbytes(s, extra_meta);
        hbyte(s, 0x00);
    }
    hbyte(s, 0xFF);

    s->pos = 0;             /* offsets count from the preamble */
    wb(s, 0xFF); wb(s, 0xFF); wb(s, 0xBD); wb(s, 0xB3);
    dummy(s, ECP5_DUMMY_AFTER_PREAMBLE);

    if (spi >= 0) {
        wb(s, CMD_SPI_MODE);
        wb(s, spimodes[spi].code);
        zeros(s, 2);
    }

    wb(s, CMD_LSC_RESET_CRC);
    zeros(s, 3);
    s->crc = 0;
    s->crc_start = s->pos;

    wb(s, CMD_VERIFY_ID);
    zeros(s, 3);
    u32(s, c->db->idcode);

    wb(s, CMD_LSC_PROG_CNTRL0);
    zeros(s, 3);
    u32(s, ctrl0);

    wb(s, CMD_LSC_INIT_ADDRESS);
    zeros(s, 3);

    if (o->compress) {
        write_compressed(s, c);
    } else {
        wb(s, CMD_LSC_PROG_INCR_RTI);
        wb(s, ECP5_CRC_META);
        wb(s, (uint8_t)(c->frames >> 8));
        wb(s, (uint8_t)c->frames);
        for (f = 0; f < c->frames; f++) {
            const uint8_t *fr = c->cram + (c->frames - 1 - f) * B;
            wbytes(s, fr, B);
            crc_out(s);
            for (i = 0; i < ECP5_DUMMY_AFTER_FRAME; i++) wb(s, 0xFF);
        }
    }

    dummy(s, ECP5_SECURITY_SED_SPACE);

    wb(s, CMD_ISC_PROGRAM_USERCODE);
    wb(s, 0x80);
    zeros(s, 2);
    u32(s, c->usercode);
    crc_out(s);

    for (br = c->bram; br; br = br->next) {
        wb(s, CMD_LSC_EBR_ADDRESS);
        zeros(s, 3);
        u32(s, br->index << 11);
        wb(s, CMD_LSC_EBR_WRITE);
        wb(s, 0xD0);
        wb(s, 0x01);            /* 256 frames of 72 bits */
        wb(s, 0x00);
        wbytes(s, br->data, (uint32_t)sizeof(br->data));
        crc_out(s);
    }

    wb(s, CMD_ISC_PROGRAM_DONE);
    zeros(s, 3);
    dummy(s, 4);
}
