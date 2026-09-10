/*
 * Graphics through libz: the tree's own zgfx.c, compiled into the
 * runtime and called from zcc-generated code.
 *
 * There is no window here and there cannot be -- sim/ has no `wm`, no
 * second process and no message delivery, so z_win_create() correctly
 * times out. What this test proves is the layer underneath: that the
 * drawing calls reach the real implementation with the right
 * arguments and put the right bits in VRAM.
 *
 * It checks that by CHECKSUMMING the framebuffer. Both compilers must
 * produce the same picture, bit for bit, which is a far stronger claim
 * than "it ran". A wrong argument order or a botched struct-by-pointer
 * would land somewhere in those 24KB and change the number.
 */

#include <stdint.h>
#include <stdbool.h>
#include "zeitlos.h"
#include "zgfx.h"
#include "zfont.h"
#include "libz.h"

const z_font_t *z_font_5x8_get(void);
const z_font_t *z_font_6x12_get(void);

/* 512x384, 1bpp, packed rows -- the GPU_PIXEL_DOUBLE layout every
 * current board builds (rtl/boards.vh) and the one sim/ models. */
#define FB      ((volatile uint32_t *)0x20000000)
#define FBWORDS 6144

static unsigned fb_hash(void) {
    /* FNV-1a over the framebuffer. Any hash would do; this one is four
     * lines and spreads a single flipped bit across the whole word,
     * so a one-pixel difference cannot cancel out. */
    unsigned h = 2166136261u;
    int i;
    for (i = 0; i < FBWORDS; i++) {
        unsigned w = FB[i];
        h ^= w & 0xff;         h *= 16777619u;
        h ^= (w >> 8) & 0xff;  h *= 16777619u;
        h ^= (w >> 16) & 0xff; h *= 16777619u;
        h ^= (w >> 24) & 0xff; h *= 16777619u;
    }
    return h;
}

static void fb_clear(void) {
    int i;
    for (i = 0; i < FBWORDS; i++) FB[i] = 0;
}

int main(void) {
    z_clip_t clip;
    int i;

    clip.x0 = 0; clip.y0 = 0; clip.x1 = 511; clip.y1 = 383;

    fb_clear();
    printf("blank %u\n", fb_hash());

    /* software path: pixels, rects, text */
    for (i = 0; i < 64; i++) z_fb_set_pixel(i * 7, i * 5, 1, &clip);
    printf("pixels %u\n", fb_hash());

    z_fb_fill_rect(20, 20, 100, 40, 1, &clip);
    z_fb_fill_rect(40, 30, 60, 20, 0, &clip);
    printf("rects %u\n", fb_hash());

    z_fb_draw_text(130, 24, "zcc + libz", 1, z_font_5x8_get(), &clip);
    z_fb_draw_text(130, 40, "the tree's own zgfx.c", 1, z_font_6x12_get(), &clip);
    printf("text %u\n", fb_hash());

    z_fb_draw_text2(130, 60, "inverted", 0, 1, z_font_5x8_get(), &clip);
    printf("text2 %u\n", fb_hash());

    /* clipping: the same draw, twice, once clipped away entirely */
    {
        z_clip_t tiny;
        unsigned before;
        tiny.x0 = 0; tiny.y0 = 0; tiny.x1 = 4; tiny.y1 = 4;
        before = fb_hash();
        z_fb_fill_rect(300, 300, 50, 50, 1, &tiny);
        printf("clipped_away %d\n", fb_hash() == before);
        z_fb_fill_rect(300, 300, 50, 50, 1, &clip);
        printf("unclipped %d\n", fb_hash() != before);
    }

    /* the hardware paths go through the simulator's models of
     * gpu_raster.v and gpu_blit.v, which are direct translations of
     * the RTL -- so this exercises the clip-and-mask discipline in
     * z_fb_hw_line() as well as the arithmetic */
    fb_clear();
    for (i = 0; i < 16; i++)
        z_fb_hw_line(10, 10, 10 + i * 30, 300, 1, &clip);
    printf("hw_lines %u\n", fb_hash());

    z_fb_hw_box(200, 100, 400, 200, 1, &clip);
    printf("hw_box %u\n", fb_hash());

    z_fb_hw_fill_rect(220, 120, 60, 30, 1);
    printf("hw_fill %u\n", fb_hash());

    return 0;
}
