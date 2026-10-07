/*
 * basic -- the bytes of a terminal (bterm.h).
 */

#include "bterm.h"
#include "../../common/zkbd.h"

int bt_in(bterm_t *t, uint8_t b, uint32_t *key) {
    if (t->esc == 1) {
        if (b == '[' || b == 'O') {
            t->esc = 2;
            return 0;
        }
        t->esc = 0;                 /* ESC then something else: Escape, */
        *key = 0x1B;                /* and the byte is lost (rare) */
        return 1;
    }
    if (t->esc == 2) {
        if (b >= '0' && b <= '9') return 0;     /* ESC [ 3 ~ and the like */
        if (b == ';') return 0;
        t->esc = 0;
        switch (b) {
        case 'A': *key = Z_KEY_UP; return 1;
        case 'B': *key = Z_KEY_DOWN; return 1;
        case 'C': *key = Z_KEY_RIGHT; return 1;
        case 'D': *key = Z_KEY_LEFT; return 1;
        default: return 0;                      /* other keys: skipped */
        }
    }
    if (t->need) {
        if ((b & 0xC0) != 0x80) {   /* broken: start again with this byte */
            t->need = 0;
            return bt_in(t, b, key);
        }
        t->cp = (t->cp << 6) | (b & 0x3F);
        if (--t->need) return 0;
        *key = t->cp;
        return 1;
    }
    if (b == 0x1B) {
        t->esc = 1;
        return 0;
    }
    if (b < 0x80) {
        *key = b;
        return 1;
    }
    if ((b & 0xE0) == 0xC0) { t->cp = b & 0x1F; t->need = 1; return 0; }
    if ((b & 0xF0) == 0xE0) { t->cp = b & 0x0F; t->need = 2; return 0; }
    if ((b & 0xF8) == 0xF0) { t->cp = b & 0x07; t->need = 3; return 0; }
    return 0;                       /* a stray continuation byte */
}

int bt_end(bterm_t *t, uint32_t *key) {
    if (t->esc != 1) return 0;
    t->esc = 0;
    *key = 0x1B;
    return 1;
}

int bt_out(uint8_t c, char out[3]) {
    /* Latin-9's eight replacements of Latin-1 characters */
    static const uint16_t swap[][2] = {
        { 0xA4, 0x20AC }, { 0xA6, 0x0160 }, { 0xA8, 0x0161 }, { 0xB4, 0x017D },
        { 0xB8, 0x017E }, { 0xBC, 0x0152 }, { 0xBD, 0x0153 }, { 0xBE, 0x0178 },
    };
    uint32_t cp = c;
    if (c < 0x80) {
        out[0] = (char)c;
        return 1;
    }
    for (unsigned i = 0; i < sizeof(swap) / sizeof(swap[0]); i++)
        if (c == swap[i][0]) cp = swap[i][1];
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
}
