/*
 * zfpga -- flash and run: install gateware in the machine's own flash,
 * and boot it through the jumploader. docs/zboot.md sec. 5.
 *
 *   zfpga flash blink.bit [-a ADDR]
 *   zfpga run   blink.bit [-a ADDR]
 *
 * `flash` writes the bitstream at ADDR, or by default in the first of
 * these it fits, each starting at a 64 KB boundary (a boot address is
 * addr[23:16]):
 *
 *   - after the core apps (the ZAR), up to the jumploader at 0x1D0000;
 *   - after Zeitlos's own gateware, up to the boot logo at 0x0F0000 --
 *     on a Mozart ML1 flashed over JTAG, 598 KB of gateware leaves
 *     0x0A0000-0x0F0000, 320 KB;
 *   - on a flash larger than 2 MB, after the jumploader -- as many
 *     designs as there is room for.
 *
 * -a ADDR puts it anywhere inside one of them. `run` is `flash`, then a jump: the kernel points
 * the jumploader at ADDR, syncs files and pulls PROGRAMN. A power
 * cycle always comes back to Zeitlos.
 *
 * Before anything is written:
 *
 *   - the file must be a bitstream for THIS FPGA: its VERIFY_ID must
 *     match the jumploader's, which was built for this die;
 *   - it must fit, touching neither the core apps nor the jumploader;
 *   - if the same bytes are already there, nothing is written, so
 *     running the same design again is immediate.
 *
 * A bitstream packed with its own boot address (-a) is flashed, with a
 * warning: its resets then go there, not back to Zeitlos.
 */

#include "zfpga.h"

/* The flash map (docs/zboot.md sec. 5). KEEP IN SYNC with
 * Z_ZAR_FLASH_OFFSET (sw/os/zar.h) and Z_JUMP_FLASH_OFFSET,
 * Z_JUMP_REGION_SIZE (sw/common/zsoc.h); release/lib/layout.py checks. */
#define ZFPGA_ZAR_OFFSET   0x140000u
#define ZFPGA_JUMP_OFFSET  0x1D0000u
#define ZFPGA_JUMP_END     0x200000u
/* ... and the gateware region: Zeitlos's own bitstream starts at
 * USERPART_START on a board with the DFU bootloader, at 0 without, and
 * must end before the boot logo (sw/bios, logo.h). */
#define ZFPGA_GW_DFU       0x040000u
#define ZFPGA_LOGO_OFFSET  0x0F0000u

#define SECTOR 4096u
#define PAGE 256u
#define ALIGN 0x10000u          /* a boot address is addr[23:16] */

typedef uint8_t (*rd_t)(void *ctx, uint32_t off);

typedef struct {
    int have_id, have_ctrl0;
    uint32_t idcode, ctrl0;
} bitinfo_t;

static uint32_t be32(rd_t rd, void *ctx, uint32_t o) {
    return (uint32_t)rd(ctx, o) << 24 | (uint32_t)rd(ctx, o + 1) << 16 |
        (uint32_t)rd(ctx, o + 2) << 8 | rd(ctx, o + 3);
}

/* The header, the preamble, then the commands up to the first frame
 * address: 0 and what was found, or -1 if it is not a bitstream. */
static int bit_info(rd_t rd, void *ctx, uint32_t max, bitinfo_t *bi) {
    uint32_t o = 2;
    zf_memset(bi, 0, sizeof(*bi));
    if (max < 16 || rd(ctx, 0) != 0xFF || rd(ctx, 1) != 0x00) return -1;
    while (o < max && rd(ctx, o) != 0xFF) {             /* the comment strings */
        while (o < max && rd(ctx, o) != 0x00) o++;
        o++;
    }
    o++;
    if (o + 4 > max || rd(ctx, o) != 0xFF || rd(ctx, o + 1) != 0xFF ||
            rd(ctx, o + 2) != 0xBD || rd(ctx, o + 3) != 0xB3)
        return -1;
    o += 4;
    while (o + 8 <= max) {
        uint8_t op = rd(ctx, o);
        if (op == 0xFF) { o++; continue; }              /* dummy bytes */
        if (op == 0x79 || op == 0x3B) { o += 4; continue; }     /* SPI mode, reset CRC */
        if (op == 0xE2) { bi->idcode = be32(rd, ctx, o + 4); bi->have_id = 1; o += 8; continue; }
        if (op == 0x22) { bi->ctrl0 = be32(rd, ctx, o + 4); bi->have_ctrl0 = 1; o += 8; continue; }
        break;                                          /* the frames begin */
    }
    return bi->have_id ? 0 : -1;
}

static uint8_t rd_mem(void *ctx, uint32_t off) { return ((const uint8_t *)ctx)[off]; }
static uint8_t rd_flash(void *ctx, uint32_t off) {
    return zio_flash_read((uint32_t)(uintptr_t)ctx + off);
}

static uint32_t rd_le32(uint32_t off) {
    return (uint32_t)zio_flash_read(off) | (uint32_t)zio_flash_read(off + 1) << 8 |
        (uint32_t)zio_flash_read(off + 2) << 16 | (uint32_t)zio_flash_read(off + 3) << 24;
}

/* Where the core apps end: the ZAR's header, then the furthest entry. */
static uint32_t zar_end(void) {
    uint32_t n, i, end = ZFPGA_ZAR_OFFSET;
    if (zio_flash_read(ZFPGA_ZAR_OFFSET) != 'Z' || zio_flash_read(ZFPGA_ZAR_OFFSET + 1) != 'A' ||
            zio_flash_read(ZFPGA_ZAR_OFFSET + 2) != 'R' || zio_flash_read(ZFPGA_ZAR_OFFSET + 3) != '1')
        return end;                                     /* no archive: nothing to avoid */
    n = rd_le32(ZFPGA_ZAR_OFFSET + 4);
    if (n > 256) zf_fatal("the core-app archive at 0x%06x claims %u entries", ZFPGA_ZAR_OFFSET, n);
    end = ZFPGA_ZAR_OFFSET + 16 + 24 * n;
    for (i = 0; i < n; i++) {
        uint32_t e = ZFPGA_ZAR_OFFSET + 16 + 24 * i;
        uint32_t last = ZFPGA_ZAR_OFFSET + rd_le32(e + 16) + rd_le32(e + 20);
        if (last > end) end = last;
    }
    return end;
}

static uint32_t align_up(uint32_t v) {
    return (v + ALIGN - 1) & ~(ALIGN - 1);
}

static int sector_erased(uint32_t s) {
    uint32_t i;
    for (i = 0; i < SECTOR; i++) if (zio_flash_read(s + i) != 0xFF) return 0;
    return 1;
}

/* Where Zeitlos's own gateware ends, or 0 if there is none to be found.
 * It starts at 0x040000 if a bitstream begins there (a board with the
 * DFU bootloader, whose own bitstream is at 0), else at 0. It ends at
 * the first wholly erased 4 KB sector -- a compressed bitstream never
 * has one -- or at a 64 KB boundary where another bitstream begins,
 * which is a user design flashed straight after it. The space from
 * there to the logo is free for user gateware: rebuilding Zeitlos's
 * gateware larger may overwrite what was put there, which is harmless. */
static uint32_t gateware_end(void) {
    uint32_t start, s;
    bitinfo_t bi;
    if (bit_info(rd_flash, (void *)(uintptr_t)ZFPGA_GW_DFU, 4096, &bi) == 0) start = ZFPGA_GW_DFU;
    else if (bit_info(rd_flash, (void *)(uintptr_t)0, 4096, &bi) == 0) start = 0;
    else return 0;
    for (s = start + SECTOR; s < ZFPGA_LOGO_OFFSET; s += SECTOR) {
        if (!(s & (ALIGN - 1)) && bit_info(rd_flash, (void *)(uintptr_t)s, 4096, &bi) == 0) return s;
        if (sector_erased(s)) return s;
    }
    return ZFPGA_LOGO_OFFSET;
}

/* The places user gateware may go, in the order the default tries them. */
typedef struct { uint32_t lo, hi; const char *what; } space_t;

static int free_spaces(const zio_flash_info_t *fi, space_t *sp) {
    int n = 0;
    uint32_t g = gateware_end();
    sp[n].lo = align_up(zar_end()); sp[n].hi = ZFPGA_JUMP_OFFSET;
    sp[n++].what = "after the core apps";
    if (g) {
        /* never below the lock, whatever the layout looked like */
        sp[n].lo = align_up(g) > fi->lock_end ? align_up(g) : fi->lock_end;
        sp[n].hi = ZFPGA_LOGO_OFFSET;
        sp[n++].what = "after Zeitlos's gateware";
    }
    if (fi->size > ZFPGA_JUMP_END) {
        sp[n].lo = ZFPGA_JUMP_END; sp[n].hi = fi->size;
        sp[n++].what = "after the jumploader";
    }
    return n;
}

static void list_spaces(const space_t *sp, int n) {
    int k;
    for (k = 0; k < n; k++)
        zf_note("  0x%06x-0x%06x  %4u KB  %s", sp[k].lo, sp[k].hi,
            sp[k].hi > sp[k].lo ? (sp[k].hi - sp[k].lo) / 1024 : 0, sp[k].what);
}

/* 0 if the image is already there, byte for byte */
static int differs(uint32_t addr, const uint8_t *d, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n; i++) if (zio_flash_read(addr + i) != d[i]) return 1;
    return 0;
}

static void write_image(uint32_t addr, const uint8_t *d, uint32_t n) {
    uint32_t s, p, r;
    uint8_t page[PAGE];
    for (s = addr; s < addr + n; s += SECTOR) {
        if ((r = zio_flash_erase(s)) != 0)
            zf_fatal("flash: erasing 0x%06x was refused (status bits %x)", s, r);
        for (p = s; p < s + SECTOR && p < addr + n; p += PAGE) {
            uint32_t k, len = (addr + n - p < PAGE) ? addr + n - p : PAGE;
            for (k = 0; k < len; k++) page[k] = d[p - addr + k];
            for (k = 0; k < len && page[k] == 0xFF; k++) ;
            if (k == len) continue;                     /* erased already */
            if ((r = zio_flash_program(p, page, len)) != 0)
                zf_fatal("flash: programming 0x%06x was refused (status bits %x)", p, r);
        }
    }
}

/* flash, and optionally run */
static int flash_cmd(int argc, char **argv, int run) {
    const char *in = NULL, *what = run ? "run" : "flash";
    uint32_t addr = 0, len;
    int have_addr = 0, i, rc, n_sp, k;
    space_t sp[3];
    uint8_t *img;
    bitinfo_t bi, jl;
    zio_flash_info_t fi;

    for (i = 1; i < argc; i++) {
        if (zf_streq(argv[i], "-a") && i + 1 < argc) {
            if (!zf_parse_uint(argv[++i], &addr)) zf_fatal("%s: -a %s: not an address", what, argv[i]);
            have_addr = 1;
        }
        else if (argv[i][0] == '-') zf_fatal("%s: unknown option %s", what, argv[i]);
        else if (!in) in = argv[i];
        else zf_fatal("%s: more than one bitstream", what);
    }
    if (!in) zf_fatal("usage: zfpga %s FILE.bit [-a ADDR]", what);

    img = zf_read_all(in, &len);
    if (!img) zf_fatal("%s: cannot read %s", what, in);
    if (bit_info(rd_mem, img, len, &bi))
        zf_fatal("%s: %s is not an ECP5 bitstream", what, in);

    rc = zio_flash_begin(&fi);
    if (rc == -1) zf_fatal("%s: no writable flash here -- this works on the machine, with gateware "
        "whose flash can be written (docs/spiflash.md)", what);
    if (rc == -2) zf_fatal("%s: another program is writing the flash", what);

    /* the jumploader was built for this die: the image must match it */
    if (bit_info(rd_flash, (void *)(uintptr_t)ZFPGA_JUMP_OFFSET, 4096, &jl) == 0) {
        if (jl.idcode != bi.idcode) {
            zio_flash_end();
            zf_fatal("%s: %s is for device ID %08x; this FPGA is %08x", what, in, bi.idcode, jl.idcode);
        }
    } else if (run) {
        zio_flash_end();
        zf_fatal("run: there is no jumploader at 0x%06x to boot through (make flash_jump)", ZFPGA_JUMP_OFFSET);
    } else {
        zf_note("warning: no jumploader at 0x%06x, so the device ID is not checked", ZFPGA_JUMP_OFFSET);
    }
    if (bi.have_ctrl0 && (bi.ctrl0 & (1u << 20)))
        zf_note("warning: %s was packed with its own boot address: its resets go there, "
            "not back to Zeitlos (a power cycle always does)", in);

    /* where: the first space it fits in, or where -a says if that is
     * inside one */
    n_sp = free_spaces(&fi, sp);
    if (have_addr && (addr & (ALIGN - 1))) {
        zio_flash_end();
        zf_fatal("%s: 0x%06x is not 64 KB aligned (a boot address is addr[23:16])", what, addr);
    }
    for (k = 0; k < n_sp; k++) {
        if (!have_addr && sp[k].lo + len <= sp[k].hi) { addr = sp[k].lo; break; }
        if (have_addr && addr >= sp[k].lo && addr + len <= sp[k].hi) break;
    }
    if (k == n_sp) {
        zio_flash_end();
        if (have_addr)
            zf_note("%s: 0x%06x-0x%06x is not free; user gateware can go in", what, addr, addr + len);
        else
            zf_note("%s: %u bytes do not fit in any free space:", what, len);
        list_spaces(sp, n_sp);
        zf_fatal("%s: nothing written", what);
    }

    if (!differs(addr, img, len)) {
        zf_note("%s: already at 0x%06x", in, addr);
    } else {
        zf_note("flashing %s at 0x%06x (%u bytes) ...", in, addr, len);
        write_image(addr, img, len);
        if (differs(addr, img, len)) {
            zio_flash_end();
            zf_fatal("%s: the flash at 0x%06x does not read back as written", what, addr);
        }
        zf_note("done, verified");
    }
    zio_flash_end();

    if (!run) return 0;
    zf_note("jumping to 0x%06x", addr);
    zio_out_close();            /* the relay must drain before the FPGA reloads */
    rc = zio_jump(addr);
    zio_out_open();
    if (rc == 0) return 0;      /* only the simulation returns from a jump */
    zf_fatal("run: the jump did not happen: %s",
        rc == -1 ? "this board's gateware cannot pull PROGRAMN" :
        rc == -2 ? "there is no jumploader" :
        rc == -3 ? "the jumploader could not be re-pointed" :
        rc == -4 ? "this gateware does not reload through a jumploader" :
        "the FPGA did not reconfigure");
    return 1;
}

int cmd_flash(int argc, char **argv) {
    return flash_cmd(argc, argv, 0);
}

int cmd_run(int argc, char **argv) {
    return flash_cmd(argc, argv, 1);
}
