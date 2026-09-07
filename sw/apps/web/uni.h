#ifndef UNI_H
#define UNI_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * UTF-8 in, printable ASCII out.
 *
 * -- why this is not optional --
 *
 * Every font in this tree covers 0x20..0x7f and nothing else
 * (sw/common/zfont_data.c: each z_font_t is declared `0x20, 0x7f`).
 * z_fb_draw_char() silently draws NOTHING for a byte outside that
 * range, so a page full of typographic quotes renders as a page full
 * of holes -- text that is present, correct, and invisible.
 *
 * Meanwhile essentially every page on the modern web is UTF-8, and
 * the ones that matter most here are the worst offenders: an English
 * Wikipedia article is full of en dashes, curly apostrophes,
 * non-breaking spaces and the occasional diacritic in a name.
 *
 * So folding is not a nicety, it is what makes the text legible at
 * all. `Wikipedia — the free encyclopedia` becomes
 * `Wikipedia -- the free encyclopedia`, which is what a 1bpp 5x8 font
 * can actually say.
 *
 * -- the rule --
 *
 * One codepoint in, zero to three ASCII bytes out:
 *
 *   - ASCII passes through unchanged.
 *   - Punctuation with an obvious ASCII ancestor becomes it: curly
 *     quotes to straight, en/em dash to - and --, ellipsis to ...,
 *     NBSP to a space, soft hyphen to nothing.
 *   - A Latin letter with a diacritic loses the diacritic. `Gödel`
 *     becomes `Godel`. That is wrong as typography and right as
 *     legibility: the alternative on this display is `G?del`.
 *   - Anything else becomes '?'.
 *
 * The last rule is deliberately not silent-drop. A dropped character
 * makes a sentence read as though it were complete when a word is
 * missing; a '?' says something was here and could not be shown,
 * which is a true statement and one the reader can act on.
 *
 * -- what is NOT here --
 *
 * No transliteration of non-Latin scripts. Greek, Cyrillic, Han and
 * the rest all become '?'. Doing better means a table measured in
 * tens of kilobytes for text that would still be unreadable, on a
 * machine where the browser is already the largest app.
 *
 * No normalisation. A combining acute after an `e` is dropped as an
 * unknown mark rather than merged into the letter, so `e` + U+0301
 * reads `e` and U+00E9 also reads `e`. Same answer by two routes,
 * which is the useful property.
 */

#include <stdint.h>
#include <stdbool.h>

// Most bytes any single codepoint can fold to (the em dash, "--",
// plus a NUL). Sized for the caller's stack buffer.
#define UNI_FOLD_MAX 4

// Decodes one UTF-8 sequence from `s` (which need not be NUL
// terminated; `len` bounds it).
//
// Returns the number of bytes consumed, always at least 1, and writes
// the codepoint to *cp. An invalid or truncated sequence consumes ONE
// byte and yields U+FFFD, which is what keeps a malformed page from
// desynchronising the rest of the document -- resynchronising on the
// next byte is the behaviour every browser converged on, and the
// alternative (consume the whole claimed length) lets a single bad
// lead byte eat real text after it.
uint32_t uni_utf8_next(const char *s, uint32_t len, uint32_t *cp);

// Folds one codepoint to ASCII, NUL-terminating `out` (>= UNI_FOLD_MAX
// bytes). Returns the number of bytes written, 0 to 3.
uint32_t uni_fold(uint32_t cp, char *out);

// True for the codepoints that count as whitespace for HTML's
// collapsing rules -- space, tab, CR, LF, form feed, and NBSP.
//
// NBSP is in this list on purpose even though HTML says it is NOT
// collapsible whitespace. It is here because it reaches this browser
// almost exclusively as a layout shim (`&nbsp;` between a number and
// its unit, or padding a table cell), and treating it as ordinary
// space is what lets those lines wrap instead of running off the
// right edge of a 640px screen. See html.h.
bool uni_is_space(uint32_t cp);

#endif
