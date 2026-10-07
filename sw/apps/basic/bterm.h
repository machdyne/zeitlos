#ifndef BTERM_H
#define BTERM_H

/*
 * basic -- the bytes of a terminal (term, through the basic0 port) to and
 * from what the BASIC computer uses. Portable: tested on a host.
 *
 * In: term sends UTF-8, and arrows as VT100 sequences (ESC [ A ...). They
 * become keysyms, the same as wm's Z_WM_KEY carries (zkbd.h): characters
 * as Unicode codepoints, Up/Down/Left/Right as Z_KEY_*. A sequence comes
 * whole in one message, so an ESC that ends a message is the Escape key.
 *
 * Out: BASIC's text is Latin-9 bytes; term wants UTF-8.
 */

#include <stdint.h>

typedef struct {
    uint32_t cp;        /* the character being decoded */
    int need;           /* UTF-8 continuation bytes still to come */
    int esc;            /* 0, 1 after ESC, 2 after ESC [ */
} bterm_t;

/* One byte in: 1 and *key if a key is complete, otherwise 0. */
int bt_in(bterm_t *t, uint8_t b, uint32_t *key);

/* The end of a message: 1 and *key (Escape) if an ESC was left alone. */
int bt_end(bterm_t *t, uint32_t *key);

/* A Latin-9 byte as UTF-8: the bytes in out, their number returned */
int bt_out(uint8_t c, char out[3]);

#endif
