/*
 * zfpga -- build: every stage in one command, from a board profile.
 *
 *   zfpga build /fpga/examples/blink.v -b lakritz
 *
 * writes blink.zl, blink.zn, blink.cfg and blink.bit beside the input:
 * each stage's output is kept, so any of them can be looked at, or edited
 * and built from (the input's extension says where to start: .v
 * synthesises, .zl places, .zn routes, .cfg or .config only packs).
 *
 * Every name is 8.3, because the card's FatFs has no long names: a
 * Trellis configuration is .cfg here, not .config, and an input whose
 * outputs would not fit is refused before anything runs.
 *
 * One process, not four commands, for two reasons. The posix shell's
 * `a && b` tests whether a STARTED, not whether it finished
 * (sw/apps/posix/sh.c, bi_run), so a chain of commands would race. And
 * the stages share the database: it is loaded once, and each stage's
 * own memory is released before the next (zf_mark / zf_release), which
 * is what fits all four in the 4MB the kernel gives zfpga.
 *
 * A BOARD PROFILE is a small text file in <db dir>/boards/, on the card
 * /fpga/boards/lakritz.brd (docs/zfpga-formats.md sec. 7):
 *
 *   device  LFE5U-25F
 *   package CABGA256
 *   lpf     lakritz.lpf          # beside the profile, or an absolute path
 *   pack    -c                   # zfpga pack's options
 *
 * With no -b, the board named in <db dir>/board (one line) is used.
 */

#include "zfpga.h"

typedef struct {
    const char *name, *device, *package, *lpf;
    char *pack[16];
    int n_pack;
} board_t;

static char *path2(const char *dir, const char *a, const char *b) {
    char buf[256];
    zf_fmt(buf, sizeof(buf), "%s/%s%s", dir, a, b);
    return zf_strdup(buf);
}

/* The tree's board names that are longer than the card's 8.3 file names
 * allow: the Makefile says `-b mozart_ml1`, the card holds mozart1.brd. */
static const struct { const char *name, *file; } board_aliases[] = {
    { "mozart_ml1", "mozart1" },
    { "sergei_ml1", "sergei1" },
};

static const char *board_file(const char *name) {
    unsigned i;
    for (i = 0; i < sizeof(board_aliases) / sizeof(board_aliases[0]); i++)
        if (zf_streq(name, board_aliases[i].name)) return board_aliases[i].file;
    return name;
}

static void read_board(const char *dbdir, const char *name, board_t *b) {
    zf_reader_t *r = zf_alloc(sizeof(*r));
    const char *path = path2(dbdir, "boards/", zf_strdup(board_file(name)));
    char *line, *tok[20], *p;
    zio_file_t *f;
    zf_memset(b, 0, sizeof(*b));
    b->name = name;
    p = zf_alloc(zf_strlen(path) + 5);
    zf_fmt(p, (int)zf_strlen(path) + 5, "%s.brd", path);
    path = p;
    f = zio_open_read(path);
    if (!f) zf_fatal("no board profile %s (boards live in %s/boards/)", path, dbdir);
    zio_close(f);
    zf_reader_open(r, path);
    while ((line = zf_reader_line(r)) != NULL) {
        int n = zf_tokens(line, tok, 20), i;
        if (!n) continue;
        if (zf_streq(tok[0], "device") && n == 2) b->device = zf_strdup(tok[1]);
        else if (zf_streq(tok[0], "package") && n == 2) b->package = zf_strdup(tok[1]);
        else if (zf_streq(tok[0], "lpf") && n == 2)
            b->lpf = tok[1][0] == '/' ? zf_strdup(tok[1]) : path2(dbdir, "boards/", zf_strdup(tok[1]));
        else if (zf_streq(tok[0], "pack")) {
            for (i = 1; i < n && b->n_pack < 16; i++) b->pack[b->n_pack++] = zf_strdup(tok[i]);
        } else {
            zf_fatal_at(path, r->lineno, "unknown line '%s' (device, package, lpf, pack)", tok[0]);
        }
    }
    zf_reader_close(r);
    if (!b->device || !b->package) zf_fatal("%s: needs device and package lines", path);
}

/* the default board: one word in <db dir>/board */
static const char *default_board(const char *dbdir) {
    const char *path = path2(dbdir, "board", "");
    zio_file_t *f = zio_open_read(path);
    char buf[64];
    int n, i = 0;
    if (!f) return NULL;
    n = zio_read(f, buf, (int)sizeof(buf) - 1);
    zio_close(f);
    if (n <= 0) return NULL;
    while (i < n && buf[i] != '\n' && buf[i] != '\r' && buf[i] != ' ') i++;
    buf[i] = 0;
    return i ? zf_strdup(buf) : NULL;
}

static int ends(const char *s, const char *ext) {
    size_t n = zf_strlen(s), e = zf_strlen(ext);
    return n > e && zf_streq(s + n - e, ext);
}

static char *swap_ext(const char *in, const char *ext) {
    size_t n = zf_strlen(in), dot = n, j;
    char *o;
    for (j = n; j > 0; j--) { if (in[j - 1] == '/') break; if (in[j - 1] == '.') { dot = j - 1; break; } }
    o = zf_alloc(dot + zf_strlen(ext) + 1);
    zf_memcpy(o, in, dot);
    zf_memcpy(o + dot, ext, zf_strlen(ext) + 1);
    return o;
}

int cmd_build(int argc, char **argv, const char *dbdir_default) {
    char **extra = 0;
    int n_extra = 0;
    const char *in = NULL, *out = NULL, *dbdir = dbdir_default, *bname = NULL;
    board_t *b = zf_alloc(sizeof(*b));
    char *zl, *zn, *cfg, *bit;
    char *av[32];
    int i, n, stage;
    zf_mark_t mark;
    zdb_t *db = zf_alloc(sizeof(*db));
    uint32_t t0 = 0, start = zio_ms();

/* Every stage says when it starts and how long it took, so a long run
 * shows where it is -- the first on-board run printed nothing for a
 * minute, and there was no telling what it was doing. */
#define STAGE_DONE(what) do { uint32_t t_ = zio_ms();                     \
        zf_note("  %s: %u.%u s, peak %u KB so far", what, (t_ - t0) / 1000u, \
            (t_ - t0) / 100u % 10u, (unsigned)(zf_mem_peak() / 1024));      \
        t0 = t_; } while (0)

    for (i = 1; i < argc; i++) {
        if (zf_streq(argv[i], "--")) { extra = argv + i + 1; n_extra = argc - i - 1; break; }
        else if (zf_streq(argv[i], "-b") && i + 1 < argc) bname = argv[++i];
        else if (zf_streq(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (zf_streq(argv[i], "-D") && i + 1 < argc) dbdir = argv[++i];
        else if (argv[i][0] == '-') zf_fatal("build: unknown option %s", argv[i]);
        else if (!in) in = argv[i];
        else zf_fatal("build: more than one input");
    }
    if (!in) zf_fatal("usage: zfpga build IN.{v,zl,zn,cfg} [-b BOARD] [-o OUT.bit] [-D DBDIR]");
    if (!dbdir) zf_fatal("no database directory; give -D DIR");
    if (!bname) bname = default_board(dbdir);
    if (!bname) zf_fatal("no board: give -b NAME, or put its name in %s/board", dbdir);
    read_board(dbdir, bname, b);

    if (ends(in, ".v")) stage = 0;
    else if (ends(in, ".zl")) stage = 1;
    else if (ends(in, ".zn")) stage = 2;
    else if (ends(in, ".cfg") || ends(in, ".config")) stage = 3;
    else zf_fatal("build: %s: start from a .v, .zl, .zn or .cfg", in);

    zl = stage <= 1 ? (stage == 1 ? (char *)in : swap_ext(in, ".zl")) : NULL;
    zn = stage <= 2 ? (stage == 2 ? (char *)in : swap_ext(in, ".zn")) : NULL;
    cfg = stage == 3 ? (char *)in : swap_ext(in, ".cfg");
    bit = out ? (char *)out : swap_ext(in, ".bit");

    /* On the card, every name this will make must be 8.3 -- refused
     * now, not after a minute of placing when the first write fails. */
    if (zio_fat83()) {
        const char *outs[4];
        int k;
        outs[0] = zl; outs[1] = zn; outs[2] = cfg; outs[3] = bit;
        for (k = 0; k < 4; k++)
            if (outs[k] && !zf_is_83(outs[k]))
                zf_fatal("build: %s is not an 8.3 name, and the card's filesystem has no long "
                    "names -- rename %s (at most 8 characters before the dot)", outs[k], in);
    }

    zf_note("board %s: %s, %s%s%s", bname, b->device, b->package,
        b->lpf ? ", pins from " : "", b->lpf ? b->lpf : "");
    t0 = zio_ms();
    zf_note("loading the chip database...");
    /* the database once, before the first mark, so no stage frees it */
    zdb_load(db, dbdir, b->device);
    mark = zf_mark();
    STAGE_DONE("database");

    if (stage <= 0) {
        n = 0;
        zf_note("synth...");
        av[n++] = "synth"; av[n++] = (char *)in; av[n++] = "-o"; av[n++] = zl;
        av[n++] = "-d"; av[n++] = (char *)b->device; av[n++] = "-p"; av[n++] = (char *)b->package;
        if (b->lpf) { av[n++] = "-l"; av[n++] = (char *)b->lpf; }
        cmd_synth(n, av);
        zf_release(mark);
        STAGE_DONE("synth");
    }
    if (stage <= 1) {
        n = 0;
        zf_note("place...");
        av[n++] = "place"; av[n++] = zl; av[n++] = "-o"; av[n++] = zn; av[n++] = "-D"; av[n++] = (char *)dbdir;
        cmd_place(n, av, dbdir);
        zf_release(mark);
        STAGE_DONE("place");
    }
    if (stage <= 2) {
        n = 0;
        zf_note("pnr...");
        av[n++] = "pnr"; av[n++] = zn; av[n++] = "-o"; av[n++] = cfg; av[n++] = "-D"; av[n++] = (char *)dbdir;
        cmd_pnr(n, av, dbdir);
        zf_release(mark);
        STAGE_DONE("pnr");
    }
    zf_note("pack...");
    n = 0;
    av[n++] = "pack"; av[n++] = cfg; av[n++] = "-o"; av[n++] = bit; av[n++] = "-D"; av[n++] = (char *)dbdir;
    for (i = 0; i < b->n_pack; i++) av[n++] = b->pack[i];
    for (i = 0; i < n_extra && n < (int)(sizeof(av) / sizeof(av[0])); i++) av[n++] = extra[i];     /* after `--` */
    cmd_pack(n, av);
    STAGE_DONE("pack");
    zf_note("wrote %s in %u.%u s (peak memory %u KB)", bit, (zio_ms() - start) / 1000u,
        (zio_ms() - start) / 100u % 10u, (unsigned)(zf_mem_peak() / 1024));
    return 0;
}

/* -- zfpga jump: a jumploader --------------------------------------------
 *
 *   zfpga jump TARGET -b BOARD [-o OUT.bit] [-D DBDIR]
 *
 * A jumploader is the smallest useful design there is: it pulls the
 * board's PROGRAMN pin low the moment it wakes, and the FPGA reloads
 * from TARGET. Zeitlos jumps to the one at 0x1D0000 to reboot, or to
 * boot other gateware, and the kernel changes where it points in place
 * (sw/common/zjump.c) -- which is what -J makes possible. The board's
 * pin constraints must name PROGRAMN. docs/zboot.md sec. 5.
 */
static const char JUMP_V[] =
    "// A jumploader (zfpga jump): pull PROGRAMN low as soon as the\n"
    "// design wakes; the FPGA then reloads from this bitstream's boot\n"
    "// address. docs/zboot.md sec. 5.\n"
    "module top(output PROGRAMN);\n"
    "    assign PROGRAMN = 1'b0;\n"
    "endmodule\n";

int cmd_jump(int argc, char **argv, const char *dbdir_default) {
    const char *target = NULL, *bname = NULL, *out = "jump.bit", *dbdir = dbdir_default;
    char *src, *av[16];
    zf_writer_t *w;
    uint32_t t;
    int i, n = 0, rc;

    for (i = 1; i < argc; i++) {
        if (zf_streq(argv[i], "-b") && i + 1 < argc) bname = argv[++i];
        else if (zf_streq(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (zf_streq(argv[i], "-D") && i + 1 < argc) dbdir = argv[++i];
        else if (argv[i][0] == '-') zf_fatal("jump: unknown option %s", argv[i]);
        else if (!target) target = argv[i];
        else zf_fatal("jump: more than one target");
    }
    if (!target) zf_fatal("usage: zfpga jump TARGET [-b BOARD] [-o OUT.bit] [-D DBDIR]");
    if (!zf_parse_uint(target, &t) || (t & 0xFFFF) || t > 0xFF0000)
        zf_fatal("jump: target %s: a flash address, 64 KB aligned, below 16 MB", target);

    /* the source, beside the output: jump.bit -> jump.v */
    src = zf_alloc(zf_strlen(out) + 4);
    zf_memcpy(src, out, zf_strlen(out) + 1);
    {
        size_t k = zf_strlen(src);
        while (k > 0 && src[k - 1] != '.' && src[k - 1] != '/') k--;
        if (k > 0 && src[k - 1] == '.') src[k - 1] = 0;
        zf_memcpy(src + zf_strlen(src), ".v", 3);
    }
    w = zf_alloc(sizeof(*w));
    zf_writer_open(w, src);
    zf_writer_bytes(w, (const uint8_t *)JUMP_V, (uint32_t)zf_strlen(JUMP_V));
    zf_writer_close(w);

    av[n++] = "build"; av[n++] = src; av[n++] = "-o"; av[n++] = (char *)out;
    if (bname) { av[n++] = "-b"; av[n++] = (char *)bname; }
    av[n++] = "-D"; av[n++] = (char *)dbdir;
    av[n++] = "--"; av[n++] = "-a"; av[n++] = (char *)target; av[n++] = "-J";
    rc = cmd_build(n, av, dbdir_default);
    zio_remove(src);
    if (!rc) zf_note("jumploader to 0x%06x: %s", t, out);
    return rc;
}
