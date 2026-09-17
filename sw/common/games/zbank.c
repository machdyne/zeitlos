/*
 * Zeitlos -- the shared casino bank, /casino.dat.
 * See zbank.h for why games apply a delta and never write a balance.
 */

#include "zbank.h"

/* -- small string helpers ---------------------------------------------
 *
 * Hand-rolled rather than snprintf, for the reason docs/app_runtime.md
 * gives: one conversion specifier links picolibc's formatter at a cost
 * of around 100KB, which is enough to push an app past the space the
 * loader has for it. Formatting a six-line file must not cost that.
 */

static bool streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void scopy(char *dst, int cap, const char *src)
{
    int i = 0;
    while (src && src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static bool is_digit(char c) { return c >= '0' && c <= '9'; }
static bool is_blank(char c) { return c == ' ' || c == '\t'; }

static int put_str(char *out, int outlen, int at, const char *s)
{
    while (s && *s) {
        if (at >= outlen - 1) return -1;
        out[at++] = *s++;
    }
    return at;
}

static int put_num(char *out, int outlen, int at, int32_t v)
{
    char tmp[12];
    int i = 0;
    uint32_t u;

    if (v < 0) {
        if (at >= outlen - 1) return -1;
        out[at++] = '-';
        u = (uint32_t)(-(int64_t)v);
    } else {
        u = (uint32_t)v;
    }

    if (u == 0) tmp[i++] = '0';
    while (u > 0 && i < 11) { tmp[i++] = (char)('0' + u % 10); u /= 10; }

    while (i > 0) {
        if (at >= outlen - 1) return -1;
        out[at++] = tmp[--i];
    }

    return at;
}

/* -- defaults and lookup ---------------------------------------------- */

void zbank_defaults(zbank_t *b)
{
    int i;

    for (i = 0; i < (int)sizeof(zbank_t); i++) ((char *)b)[i] = 0;

    b->version = ZBANK_VERSION;
    b->chips = ZBANK_START;
    b->peak = ZBANK_START;
    b->debt = 0;
    b->buyins = 0;
    b->ngames = 0;
}

zbank_game_t *zbank_game(zbank_t *b, const char *name)
{
    int i;

    for (i = 0; i < b->ngames; i++)
        if (streq(b->game[i].name, name)) return &b->game[i];

    if (b->ngames >= ZBANK_MAX_GAMES) return 0;

    scopy(b->game[b->ngames].name, ZBANK_NAME_MAX, name);
    b->game[b->ngames].net = 0;
    b->game[b->ngames].rounds = 0;

    return &b->game[b->ngames++];
}

int32_t zbank_owed(int32_t amount)
{
    if (amount <= 0) return 0;
    return (amount * ZBANK_LOAN_NUM + ZBANK_LOAN_DEN - 1) / ZBANK_LOAN_DEN;
}

int32_t zbank_net(const zbank_t *b)
{
    return b->chips - b->debt;
}

void zbank_apply(zbank_t *b, const char *game, int32_t delta)
{
    zbank_game_t *g = zbank_game(b, game);

    b->chips += delta;

    /* A balance cannot go below zero. A game that stakes more than the
     * bank holds is a bug in that game, not something to record here
     * as a debt -- and a negative balance would quietly break every
     * "can you afford this" check downstream. */
    if (b->chips < 0) b->chips = 0;

    if (b->chips > b->peak) b->peak = b->chips;

    if (g) {
        g->net += delta;
        g->rounds++;
    }
}

/* -- parsing ----------------------------------------------------------
 *
 * Lenient by design, per zbank.h: unknown lines are skipped, missing
 * keys keep their defaults, and a file truncated by a power cut still
 * yields everything before the cut. The one thing that is NOT
 * tolerated is a non-empty file with no balance in it at all, because
 * treating that as a fresh bank would overwrite a real one.
 */

static bool parse_int(const char *s, int n, int32_t *out)
{
    int i = 0;
    int32_t v = 0;
    bool neg = false;

    while (i < n && is_blank(s[i])) i++;
    if (i < n && (s[i] == '-' || s[i] == '+')) { neg = s[i] == '-'; i++; }
    if (i >= n || !is_digit(s[i])) return false;

    for (; i < n && is_digit(s[i]); i++) {
        v = v * 10 + (s[i] - '0');
        if (v > 1000000000) return false;
    }

    while (i < n && is_blank(s[i])) i++;
    if (i != n) return false;          /* trailing rubbish */

    *out = neg ? -v : v;
    return true;
}

int zbank_parse(const char *text, int len, zbank_t *b)
{
    int i = 0;
    bool saw_chips = false;
    bool any = false;

    zbank_defaults(b);

    if (!text || len <= 0) return ZBANK_OK;

    while (i < len) {
        int ls = i, le, ks, ke, vs;
        char key[48];
        int kn;
        int32_t v;

        while (i < len && text[i] != '\n') i++;
        le = i;
        if (i < len) i++;                 /* step over the newline */

        while (ls < le && is_blank(text[ls])) ls++;
        while (le > ls && (text[le - 1] == '\r' || is_blank(text[le - 1]))) le--;

        if (ls >= le) continue;
        if (text[ls] == '#') continue;

        any = true;

        ks = ls;
        ke = ks;
        while (ke < le && text[ke] != ':' && text[ke] != '=' &&
            !is_blank(text[ke])) ke++;

        kn = ke - ks;
        if (kn <= 0 || kn >= (int)sizeof key) continue;

        {
            int k;
            for (k = 0; k < kn; k++) key[k] = text[ks + k];
            key[kn] = '\0';
        }

        vs = ke;
        while (vs < le && (is_blank(text[vs]) || text[vs] == ':' ||
            text[vs] == '=')) vs++;

        if (!parse_int(text + vs, le - vs, &v)) continue;

        if (streq(key, "version")) { b->version = (uint32_t)v; continue; }
        if (streq(key, "chips")) { b->chips = v; saw_chips = true; continue; }
        if (streq(key, "peak")) { b->peak = v; continue; }
        if (streq(key, "buyins")) { b->buyins = (uint32_t)v; continue; }
        if (streq(key, "debt")) { b->debt = v; continue; }

        /* game.<name>.net / game.<name>.rounds */
        if (key[0] == 'g' && key[1] == 'a' && key[2] == 'm' &&
            key[3] == 'e' && key[4] == '.') {
            char name[ZBANK_NAME_MAX];
            int p = 5, q = 5;
            zbank_game_t *g;

            while (key[q] && key[q] != '.') q++;
            if (!key[q]) continue;
            if (q - p >= ZBANK_NAME_MAX) continue;

            {
                int k;
                for (k = 0; k < q - p; k++) name[k] = key[p + k];
                name[q - p] = '\0';
            }

            g = zbank_game(b, name);
            if (!g) continue;

            if (streq(key + q + 1, "net")) g->net = v;
            else if (streq(key + q + 1, "rounds")) g->rounds = (uint32_t)v;
            continue;
        }
    }

    if (b->chips < 0) b->chips = 0;
    if (b->debt < 0) b->debt = 0;
    if (b->peak < b->chips) b->peak = b->chips;

    /* Content, but no balance anywhere in it. See zbank.h. */
    if (any && !saw_chips) return ZBANK_CORRUPT;

    return ZBANK_OK;
}

int zbank_format(const zbank_t *b, char *out, int outlen)
{
    int at = 0, i;

    at = put_str(out, outlen, at,
        "# Zeitlos casino bank -- chips shared by every game.\n"
        "# See sw/common/games/zbank.h. Safe to edit by hand.\n");
    if (at < 0) return -1;

    at = put_str(out, outlen, at, "version: ");
    if (at >= 0) at = put_num(out, outlen, at, (int32_t)b->version);
    if (at >= 0) at = put_str(out, outlen, at, "\nchips: ");
    if (at >= 0) at = put_num(out, outlen, at, b->chips);
    if (at >= 0) at = put_str(out, outlen, at, "\npeak: ");
    if (at >= 0) at = put_num(out, outlen, at, b->peak);
    if (at >= 0) at = put_str(out, outlen, at, "\ndebt: ");
    if (at >= 0) at = put_num(out, outlen, at, b->debt);
    if (at >= 0) at = put_str(out, outlen, at, "\nbuyins: ");
    if (at >= 0) at = put_num(out, outlen, at, (int32_t)b->buyins);
    if (at >= 0) at = put_str(out, outlen, at, "\n");
    if (at < 0) return -1;

    for (i = 0; i < b->ngames; i++) {
        at = put_str(out, outlen, at, "game.");
        if (at >= 0) at = put_str(out, outlen, at, b->game[i].name);
        if (at >= 0) at = put_str(out, outlen, at, ".net: ");
        if (at >= 0) at = put_num(out, outlen, at, b->game[i].net);
        if (at >= 0) at = put_str(out, outlen, at, "\ngame.");
        if (at >= 0) at = put_str(out, outlen, at, b->game[i].name);
        if (at >= 0) at = put_str(out, outlen, at, ".rounds: ");
        if (at >= 0) at = put_num(out, outlen, at, (int32_t)b->game[i].rounds);
        if (at >= 0) at = put_str(out, outlen, at, "\n");
        if (at < 0) return -1;
    }

    out[at] = '\0';
    return at;
}

const char *zbank_strerror(int rv)
{
    switch (rv) {
    case ZBANK_OK:      return "ok";
    case ZBANK_MISSING: return "no bank yet";
    case ZBANK_CORRUPT: return "the bank file is damaged";
    case ZBANK_IOERR:   return "the bank could not be written";
    case ZBANK_REFUSED: return "refused";
    }
    return "?";
}

/* -- the filesystem half ----------------------------------------------
 *
 * Kept behind ZBANK_NO_FS so the host tests can link everything above
 * without a filesystem. The tests exercise the parser, the formatter
 * and the arithmetic, which is where anything interesting happens; the
 * three functions below are a read, a write and a call to the pure
 * half.
 */

#ifndef ZBANK_NO_FS

#include "../zfsapp.h"

int zbank_load(zbank_t *b)
{
    /* A caller-owned buffer, not fs_mallocfile(). An app's heap and
     * stack share one 16KB allowance and _sbrk() refuses to grow into
     * the stack, so a few KB of malloc can fail on the device while
     * succeeding on any build machine -- sw/apps/settings found this
     * the hard way (sw/common/zfsapp.h). */
    static char buf[ZBANK_FILE_MAX];
    int n;

    zbank_defaults(b);

    {
        /* The old location, read only if the new one is not there. A
         * card that has both has already been saved since the move, so
         * the new file is the live one. */
        const char *path = ZBANK_PATH;

        if (fs_size((char *)ZBANK_PATH) <= 0) {
            if (fs_size((char *)ZBANK_OLD_PATH) <= 0) return ZBANK_MISSING;
            path = ZBANK_OLD_PATH;
        }

        n = fs_read_file((char *)path, buf, sizeof buf);
    }
    if (n < 0) return ZBANK_IOERR;
    if (n == 0) return ZBANK_MISSING;

    return zbank_parse(buf, n, b);
}

int zbank_save(const zbank_t *b)
{
    static char buf[ZBANK_FILE_MAX];
    int n;

    n = zbank_format(b, buf, sizeof buf);
    if (n < 0) return ZBANK_IOERR;

    if (fs_write_file((char *)ZBANK_PATH, buf, n) == n) return ZBANK_OK;

    /* A first save on a card with no /USER. Creating it eagerly on
     * every save would be a directory lookup per round for a condition
     * that is true once in the life of a card; doing it only when the
     * write has actually failed costs nothing on the path that matters.
     *
     * fs_mkdir() failing is not checked separately -- if it fails
     * because the directory already exists, the retry is still the
     * right thing to do, and if it fails for any other reason the retry
     * will fail too and report it. */
    fs_mkdir(ZBANK_DIR);

    if (fs_write_file((char *)ZBANK_PATH, buf, n) != n) return ZBANK_IOERR;

    return ZBANK_OK;
}

int zbank_adjust(const char *game, int32_t delta, int32_t *out_chips)
{
    zbank_t b;
    int rv = zbank_load(&b);

    /* MISSING is fine -- a first round creates the file. CORRUPT is
     * not: writing here would replace a real balance with a default
     * one, which is the single unrecoverable outcome. */
    if (rv == ZBANK_CORRUPT || rv == ZBANK_IOERR) return rv;

    zbank_apply(&b, game, delta);

    rv = zbank_save(&b);
    if (rv != ZBANK_OK) return rv;

    if (out_chips) *out_chips = b.chips;
    return ZBANK_OK;
}

int zbank_borrow(int32_t amount, int32_t *out_chips)
{
    zbank_t b;
    int32_t owed;
    int rv;

    if (amount <= 0) return ZBANK_REFUSED;

    rv = zbank_load(&b);
    if (rv == ZBANK_CORRUPT || rv == ZBANK_IOERR) return rv;

    /* The vig, charged once at the counter. Rounded UP, so borrowing
     * one chip still costs something -- a rate that rounds to nothing
     * on small loans is not a rate. */
    owed = zbank_owed(amount);

    if (b.debt + owed > ZBANK_MAX_DEBT) return ZBANK_REFUSED;

    b.chips += amount;
    b.debt += owed;
    b.buyins++;
    if (b.chips > b.peak) b.peak = b.chips;

    rv = zbank_save(&b);
    if (rv != ZBANK_OK) return rv;

    if (out_chips) *out_chips = b.chips;
    return ZBANK_OK;
}

int zbank_repay(int32_t amount, int32_t *out_chips, int32_t *out_paid)
{
    zbank_t b;
    int32_t pay;
    int rv = zbank_load(&b);

    if (out_paid) *out_paid = 0;
    if (rv == ZBANK_CORRUPT || rv == ZBANK_IOERR) return rv;
    if (amount <= 0) return ZBANK_REFUSED;

    /* No more than is owed, and no more than is held. Clamping rather
     * than refusing means "repay everything" is just a big number. */
    pay = amount;
    if (pay > b.debt) pay = b.debt;
    if (pay > b.chips) pay = b.chips;
    if (pay <= 0) return ZBANK_REFUSED;

    b.chips -= pay;
    b.debt -= pay;

    rv = zbank_save(&b);
    if (rv != ZBANK_OK) return rv;

    if (out_chips) *out_chips = b.chips;
    if (out_paid) *out_paid = pay;
    return ZBANK_OK;
}

int zbank_buyin(int32_t *out_chips)
{
    zbank_t b;
    int rv = zbank_load(&b);

    if (rv == ZBANK_CORRUPT || rv == ZBANK_IOERR) return rv;

    /* A top-up that works at any balance is a cheat with a friendly
     * name. */
    if (b.chips > 0) {
        if (out_chips) *out_chips = b.chips;
        return ZBANK_OK;
    }

    return zbank_borrow(ZBANK_START, out_chips);
}

int zbank_reset(void)
{
    zbank_t b;
    zbank_defaults(&b);
    return zbank_save(&b);
}

#endif /* ZBANK_NO_FS */
