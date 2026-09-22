/*
 * zfpga-simflash: zfpga flash and run against a flash IMAGE FILE, for
 * tests/run.sh. Named by $ZFPGA_SIMFLASH; its size is the flash size.
 * The rules are the hardware's: an erase sets a 4 KB sector to FF,
 * programming only clears bits, and nothing below 0x040000 may be
 * erased or programmed (rtl/spiflash.v). A jump is not performed: it is
 * written to $ZFPGA_SIMFLASH.jump, and returns 0.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zfpga.h"

#define LOCK_END 0x040000u

static uint8_t *img;
static uint32_t size;
static const char *path;

static void save(void) {
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(img, 1, size, f) != size) { perror(path); exit(2); }
    fclose(f);
}

int zio_flash_begin(zio_flash_info_t *info) {
    FILE *f;
    long n;
    path = getenv("ZFPGA_SIMFLASH");
    if (!path || !(f = fopen(path, "rb"))) return -1;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    size = (uint32_t)n;
    img = malloc(size);
    if (fread(img, 1, size, f) != size) { fclose(f); return -1; }
    fclose(f);
    info->size = size;
    info->id = 0x00EF4000u | (uint32_t)(size == (4u << 20) ? 0x16 : size == (16u << 20) ? 0x18 : 0x15);
    info->lock_end = LOCK_END;
    return 0;
}

void zio_flash_end(void) {
    if (img) save();
}

uint8_t zio_flash_read(uint32_t off) {
    if (!img) {                         /* reads before begin: load for them */
        zio_flash_info_t fi;
        if (zio_flash_begin(&fi)) return 0xFF;
    }
    return off < size ? img[off] : 0xFF;
}

uint32_t zio_flash_erase(uint32_t off) {
    if (off < LOCK_END) return 1u << 2;
    memset(img + (off & ~0xFFFu), 0xFF, 4096);
    return 0;
}

uint32_t zio_flash_program(uint32_t off, const uint8_t *p, uint32_t n) {
    uint32_t i;
    if (off < LOCK_END) return 1u << 2;
    if (n == 0 || n > 256 || (off & 0xFF) + n > 256) return 1u << 4;
    for (i = 0; i < n; i++) img[off + i] &= p[i];      /* NOR: 1 -> 0 only */
    return 0;
}

int zio_jump(uint32_t addr) {
    char p[512];
    FILE *f;
    snprintf(p, sizeof(p), "%s.jump", path);
    if ((f = fopen(p, "w"))) { fprintf(f, "0x%06x\n", addr); fclose(f); }
    return 0;
}
