/*
 * basic -- host tests for the terminal bytes (bterm.c): UTF-8 both ways,
 * VT100 arrows, and an Escape key alone.
 */

#include <stdio.h>
#include <string.h>
#include "../bterm.h"
#include "../bedit.h"
#include "../../../common/zkbd.h"

static int failures;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %d: ", __LINE__); \
    printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

/* a message's bytes, as keys */
static int keys(bterm_t *t, const char *msg, uint32_t *k) {
    int n = 0;
    for (const char *p = msg; *p; p++) n += bt_in(t, (uint8_t)*p, &k[n]);
    n += bt_end(t, &k[n]);
    return n;
}

int main(void) {
    bterm_t t = { 0 };
    uint32_t k[16];
    int n;

    n = keys(&t, "A\r\x7f", k);
    CHECK(n == 3 && k[0] == 'A' && k[1] == '\r' && k[2] == 0x7F, "ASCII, Enter, Backspace");
    n = keys(&t, "\xc3\xbc\xe2\x82\xac", k);        /* u-umlaut, euro */
    CHECK(n == 2 && k[0] == 0xFC && k[1] == 0x20AC, "UTF-8: %d %x %x", n, k[0], k[1]);
    CHECK(be_latin9(k[0]) == 0xFC && be_latin9(k[1]) == 0xA4, "... to Latin-9");
    n = keys(&t, "\xe2\x82", k);                    /* split across messages */
    CHECK(n == 0, "half a character: nothing yet");
    n = keys(&t, "\xac", k);
    CHECK(n == 1 && k[0] == 0x20AC, "the rest, in the next message");
    n = keys(&t, "\x1b[A\x1b[D", k);
    CHECK(n == 2 && k[0] == Z_KEY_UP && k[1] == Z_KEY_LEFT, "arrows");
    n = keys(&t, "\x1b[3~X", k);                    /* Delete: skipped */
    CHECK(n == 1 && k[0] == 'X', "other sequences skipped: %d", n);
    n = keys(&t, "\x1b", k);
    CHECK(n == 1 && k[0] == 0x1B, "Escape alone, at the end of a message");
    n = keys(&t, "\x03", k);
    CHECK(n == 1 && k[0] == 3, "Ctrl-C");

    char o[3];
    CHECK(bt_out('A', o) == 1 && o[0] == 'A', "out: ASCII");
    CHECK(bt_out(0xFC, o) == 2 && !memcmp(o, "\xc3\xbc", 2), "out: u-umlaut");
    CHECK(bt_out(0xA4, o) == 3 && !memcmp(o, "\xe2\x82\xac", 3), "out: Latin-9 euro");
    CHECK(bt_out(0xBD, o) == 2 && !memcmp(o, "\xc5\x93", 2), "out: oe");
    /* every Latin-9 character round trips */
    int bad = 0;
    for (int c = 0x20; c < 0x100; c++) {
        if (c == 0x7F || (c >= 0x80 && c < 0xA0)) continue;
        n = bt_out((uint8_t)c, o);
        bterm_t u = { 0 };
        uint32_t back = 0;
        int got = 0;
        for (int i = 0; i < n; i++) got += bt_in(&u, (uint8_t)o[i], &back);
        if (got != 1 || be_latin9(back) != c) bad++;
    }
    CHECK(!bad, "every Latin-9 character round trips: %d do not", bad);

    if (failures) {
        printf("FAILED: %d\n", failures);
        return 1;
    }
    printf("all terminal tests passed\n");
    return 0;
}
