/*
 * basic -- line input at the cursor (bedit.h).
 */

#include <string.h>
#include "bedit.h"
#include "../../common/zkbd.h"

uint8_t be_latin9(uint32_t cp) {
    /* Latin-9 is Latin-1 with eight characters replaced */
    static const struct { uint16_t cp; uint8_t b; } swap[] = {
        { 0x20AC, 0xA4 }, { 0x0160, 0xA6 }, { 0x0161, 0xA8 }, { 0x017D, 0xB4 },
        { 0x017E, 0xB8 }, { 0x0152, 0xBC }, { 0x0153, 0xBD }, { 0x0178, 0xBE },
    };
    for (unsigned i = 0; i < sizeof(swap) / sizeof(swap[0]); i++) {
        if (cp == swap[i].cp) return swap[i].b;
        if (cp == swap[i].b) return 0;      /* the Latin-1 characters they replace */
    }
    if (cp >= 0x20 && cp < 0x7F) return (uint8_t)cp;
    if (cp >= 0xA0 && cp <= 0xFF) return (uint8_t)cp;
    return 0;
}

void be_init(bedit_t *e, bscreen_t *s) {
    memset(e, 0, sizeof(*e));
    e->s = s;
}

void be_start(bedit_t *e) {
    e->len = 0;
    e->line[0] = 0;
}

static void echo(bedit_t *e, uint8_t c) {
    if (e->out) e->out(c);
    else bs_putc(e->s, c);
}

static void erase(bedit_t *e) {
    while (e->len) {
        e->len--;
        echo(e, '\b');
    }
    e->line[0] = 0;
}

static void add(bedit_t *e, uint8_t c) {
    if (e->len >= BE_MAX) return;
    e->line[e->len++] = (char)c;
    e->line[e->len] = 0;
    echo(e, c);
}

int be_key(bedit_t *e, uint32_t k) {
    if (k == '\r' || k == '\n') {
        echo(e, '\r');
        echo(e, '\n');
        if (e->len) memcpy(e->last, e->line, (size_t)e->len + 1);
        return 1;
    }
    if (k == 0x7F || k == '\b') {
        if (e->len) {
            e->line[--e->len] = 0;
            echo(e, '\b');
        }
        return 0;
    }
    if (k == 0x1B) {
        erase(e);
        return 0;
    }
    if (k == Z_KEY_UP) {
        erase(e);
        for (const char *p = e->last; *p; p++) add(e, (uint8_t)*p);
        return 0;
    }
    uint8_t c = be_latin9(k);
    if (c) add(e, c);
    return 0;
}
