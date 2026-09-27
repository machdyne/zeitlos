/*
 * No speech on the build machine, for host tests and renders that link
 * the widget layer. zwidget.c announces focus changes through
 * sw/common/zspeak.c, which asks the kernel -- and the cal host tests
 * and the web browser's `make render` have no scripted kernel behind
 * the syscall pointer, so linking the real zspeak.c there jumps
 * through address 0xc. These report "no speech", which is what a
 * machine without the tts service reports too.
 */
#include <stdbool.h>
#include <stdint.h>
#include "zspeak.h"

bool z_speak(const char *text, uint32_t flags) { (void)text; (void)flags; return false; }
void z_speak_cat(char *buf, uint32_t size, const char *s) { (void)buf; (void)size; (void)s; }
void z_speak_num(char *buf, uint32_t size, int32_t v) { (void)buf; (void)size; (void)v; }
bool z_speak_available(void) { return false; }
