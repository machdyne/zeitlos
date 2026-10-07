#ifndef BEDIT_H
#define BEDIT_H

/*
 * basic -- line input at the cursor (the BASIC computer's typing). Keys
 * are Zeitlos keysyms (zkbd.h): characters as Unicode, named keys from
 * Z_KEY_NAMED_BASE. Typed characters go to the screen as Latin-9, the
 * screen's and BASIC's character set. Portable: tested on a host.
 *
 *   a character   added, if it has a Latin-9 byte and the line has room
 *   Backspace     erases the last character
 *   Up            recalls the last line entered
 *   Escape        erases the whole line
 *   Enter         ends it: be_key() returns 1, and e->line holds it
 *
 * The echo goes to the screen, or through e->out if it is set (the app
 * sends it to a terminal as well).
 */

#include <stdint.h>
#include "bscreen.h"

#define BE_MAX  127     /* characters in a line (BASIC reads 127) */

typedef struct {
    bscreen_t *s;
    void (*out)(uint8_t c);     /* echo: if set, instead of bs_putc() */
    char line[BE_MAX + 1];
    int len;
    char last[BE_MAX + 1];  /* the previous line, for Up */
} bedit_t;

void be_init(bedit_t *e, bscreen_t *s);
void be_start(bedit_t *e);                  /* a new, empty line */
int be_key(bedit_t *e, uint32_t keysym);    /* 1: the line is complete */

/* Unicode codepoint to its Latin-9 byte; 0 if Latin-9 has none */
uint8_t be_latin9(uint32_t cp);

#endif
