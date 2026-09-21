/* Driver for tests/prof/prof.py: renders NV frames of a vowel and NF of a
 * fricative, then stops the emulator. */
#include "synth.h"
static int16_t out[SYNTH_FRAME];
volatile int done;
void _start(void) {
    synth_init();
    synth_set_volume(200);
    synth_frame_t f = {0};
    f.f0 = 110 * 16; f.av = 55; f.f1 = 500; f.f2 = 1500; f.f3 = 2500;
    f.b1 = 60; f.b2 = 90; f.b3 = 150;
    for (int k = 0; k < NV; k++) synth_render(&f, out, SYNTH_FRAME);      /* voiced vowel */
    f.av = 0; f.af = 60; f.a3 = 45; f.a4 = 52; f.a5 = 58;
    for (int k = 0; k < NF; k++) synth_render(&f, out, SYNTH_FRAME);      /* frication */
    done = 1;
    __asm__ volatile("ebreak");
    for (;;) ;
}
