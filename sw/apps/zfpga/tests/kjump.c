/*
 * The kernel's jumploader code (sw/os/jumpapi.c, sw/common/zjump.c),
 * on the host, against a simulated W25Q16: tests/run.sh builds and
 * runs it. The flash obeys NOR rules -- programming only clears bits,
 * only an erase sets them -- and refuses anything below 0x040000, as
 * rtl/spiflash.v does.
 *
 *   kjump J0.bit J190000.bit
 *
 * J0 and J190000 are `zfpga jump` jumploaders to 0 and 0x190000.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* flashapi.h's interface, simulated */
#define FLASH_SIZE (2u * 1024 * 1024)
static uint8_t flash[FLASH_SIZE];
static int n_erase, n_prog, violations, in_session;

bool k_flash_present(void) { return true; }
uint32_t k_flash_hw_status(void) { return 1u << 1; }          /* done, never busy */
uint8_t k_flash_window(uint32_t off) { return flash[off % FLASH_SIZE]; }
bool k_flash_begin(uint32_t owner) { (void)owner; in_session = 1; return true; }
void k_flash_end(uint32_t owner) { (void)owner; in_session = 0; }
uint32_t k_flash_hw_erase(uint32_t addr) {
    if (addr < 0x040000) { violations++; return 1u << 2; }
    if (!in_session) violations++;
    memset(flash + (addr & ~0xFFFu), 0xFF, 4096);
    n_erase++;
    return 0;
}
uint32_t k_flash_hw_program(uint32_t addr, const uint8_t *buf, uint32_t len) {
    uint32_t i;
    if (addr < 0x040000) { violations++; return 1u << 2; }
    if (!in_session || len == 0 || len > 256 || (addr & 0xFF) + len > 256) violations++;
    for (i = 0; i < len; i++) flash[addr + i] &= buf[i];       /* NOR: 1 -> 0 only */
    n_prog++;
    return 0;
}

#include "../../../common/zjump.h"
static uint8_t rd_mem(void *ctx, uint32_t off) { return ((const uint8_t *)ctx)[off]; }

/* the kernel's memory pool (sw/os/mem.c), which jumpapi.c borrows its
 * sector buffers from */
void *k_mem_alloc(uint32_t n) { return malloc(n); }
void k_mem_free(void *p) { free(p); }

int k_jump_read(uint32_t *target);
int k_jump_point(uint32_t target);

static uint8_t *load(const char *p, long *n) {
    FILE *f = fopen(p, "rb");
    uint8_t *b;
    if (!f) { perror(p); exit(2); }
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    b = malloc((size_t)*n);
    if (fread(b, 1, (size_t)*n, f) != (size_t)*n) exit(2);
    fclose(f);
    return b;
}

static int fails, checks;
static void check(int ok, const char *what) {
    checks++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) fails++;
}

/* One run: j0 at 0x1D0000, re-pointed to 0x190000 (= j1) and back. */
static void run(const uint8_t *j0, long n0, const uint8_t *j1, long n1, const char *label) {
    uint8_t *before;
    uint32_t t = 0xDEAD, i, lo = 0xFFFFFFFF, hi = 0;
    int outside;
    char msg[160];

    printf("-- %s\n", label);
    n_erase = n_prog = violations = 0;
    memset(flash, 0xFF, sizeof(flash));
    check(k_jump_read(&t) == -1, "an empty region: no jumploader");
    check(k_jump_point(0) == -1 && n_erase == 0, "... and nothing to re-point, nothing written");

    for (i = 0; i < FLASH_SIZE; i++) flash[i] = (uint8_t)(i * 7 + (i >> 11));   /* not blank */
    memset(flash + 0x1D0000, 0xFF, 0x30000);
    memcpy(flash + 0x1D0000, j0, (size_t)n0);
    before = malloc(FLASH_SIZE);
    memcpy(before, flash, FLASH_SIZE);

    check(k_jump_read(&t) == 0 && t == 0, "reads the jumploader: it points at 0");
    check(k_jump_point(0) == 0 && n_erase == 0 && n_prog == 0, "re-pointing to where it points: no flash work");
    check(k_jump_point(0x123456) == -1 && n_erase == 0, "a target that is not 64 KB aligned: refused");

    check(k_jump_point(0x190000) == 0, "re-point to 0x190000");
    check(k_jump_read(&t) == 0 && t == 0x190000, "... and it reads back 0x190000");
    check(n0 == n1 && !memcmp(flash + 0x1D0000, j1, (size_t)n1),
        "... byte-identical to zfpga's own jumploader to 0x190000");
    outside = 0;
    for (i = 0; i < FLASH_SIZE; i++) {
        if (flash[i] != before[i]) { if (i < lo) lo = i; if (i > hi) hi = i; }
        if (flash[i] != before[i] && (i < 0x1D0000 || i >= 0x1D0000 + (uint32_t)n1)) outside++;
    }
    snprintf(msg, sizeof(msg), "... nothing outside it changed (changes at 0x%06x-0x%06x; %d sector%s erased, %d pages programmed)",
        lo, hi, n_erase, n_erase == 1 ? "" : "s", n_prog);
    check(outside == 0, msg);
    check(violations == 0, "... every erase and program in a session, above the lock, within a page");

    check(k_jump_point(0) == 0 && k_jump_read(&t) == 0 && t == 0 &&
        !memcmp(flash, before, FLASH_SIZE), "and back to 0: the whole flash as it began");
    free(before);
}

/* The same jumploader with its header comment `extra` bytes longer:
 * the descriptor counts from the preamble, so it stays valid, and
 * everything after the header moves. */
static uint8_t *longer(const uint8_t *j, long n, long extra, long *nn) {
    uint8_t *o = malloc((size_t)(n + extra));
    memcpy(o, j, 2);                            /* FF 00 */
    memset(o + 2, 'x', (size_t)extra);          /* a longer first comment */
    memcpy(o + 2 + extra, j + 2, (size_t)(n - 2));
    *nn = n + extra;
    return o;
}

int main(int argc, char **argv) {
    long n0, n1, m0, m1, extra;
    uint8_t *j0, *j1;
    if (argc != 3) { fprintf(stderr, "usage: kjump J0.bit J190000.bit\n"); return 2; }
    j0 = load(argv[1], &n0);
    j1 = load(argv[2], &n1);
    run(j0, n0, j1, n1, "as built");
    /* shift the patched bytes across a 4 KB boundary: the two-sector path */
    {
        zjump_t j;
        uint32_t first, last;
        zjump_parse(rd_mem, (void *)j0, 4096, &j);
        zjump_span(&j, &first, &last);
        first += 0x1D0000; last += 0x1D0000;
        extra = (long)(((last | 0xFFF) + 1) - (last - first) / 2 - first);
    }
    {
        /* built first: an argument that sets m0 cannot also read it */
        uint8_t *s0 = longer(j0, n0, extra, &m0), *s1 = longer(j1, n1, extra, &m1);
        run(s0, m0, s1, m1, "shifted to straddle a sector boundary");
    }
    /* What openFPGALoader writes for a .bit: from its _endHeader, the byte
     * four before the preamble's B3 (latticeBitParser.cpp), so the header
     * -- and the ZJUMP1 line in it -- is gone. The kernel must say so, and
     * write nothing. */
    {
        long b3 = 0;
        while (b3 + 3 < n0 && !(j0[b3] == 0xFF && j0[b3 + 1] == 0xFF && j0[b3 + 2] == 0xBD && j0[b3 + 3] == 0xB3)) b3++;
        b3 += 3;                                    /* the B3 */
        printf("-- a .bit as openFPGALoader writes it: the header stripped\n");
        n_erase = n_prog = 0;
        memset(flash, 0xFF, sizeof(flash));
        memcpy(flash + 0x1D0000, j0 + (b3 - 4), (size_t)(n0 - (b3 - 4)));
        check(k_jump_read(NULL) == -2, "read: a jumploader whose header was stripped (-2), not 'none'");
        check(k_jump_point(0x190000) == -1 && n_erase == 0 && n_prog == 0,
            "re-pointing it is refused, and nothing is written");
        memset(flash + 0x1D0000, 0xFF, 0x30000);
        check(k_jump_read(NULL) == -1, "an erased region is still 'none' (-1)");
    }

    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
