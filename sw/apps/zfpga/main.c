/*
 * zfpga -- FPGA bitstreams built on Zeitlos. See docs/zfpga.md.
 *
 *   zfpga pack IN.config [-o OUT.bit] [-c] [-a BOOTADDR] [-f FREQ]
 *              [-m SPIMODE] [-u USERCODE] [-i IDCODE] [--background]
 *              [-D DBDIR]
 *   zfpga info DEVICE [-D DBDIR]
 *
 * The pack options are ecppack's, spelled short, because the Zeitlos
 * launch argument is 96 bytes (Z_WM_ARG_MAX) and because matching
 * ecppack option-for-option is what makes the differential test honest.
 */

#include "../../common/zargs.h"	/* quoting -- docs/posix.md */
#include "zfpga.h"

#ifndef ZFPGA_HOSTED
#define ZFPGA_HOSTED 1
#endif

/* The card layout is the search path, as for zcc's /libz. The host has
 * no default -- a path that is right on one machine is silently wrong on
 * every other. */
#if ZFPGA_HOSTED
#define DEFAULT_DB NULL
#else
#define DEFAULT_DB "/fpga"
#endif

static void usage(void) __attribute__((noreturn));
static void usage(void) {
    zio_out(
        "usage: zfpga build IN.{v,zl,zn,cfg} [-b BOARD] [-o OUT.bit] [-D DBDIR]\n"
        "       zfpga pack IN.config [-o OUT.bit] [-c] [-a BOOTADDR] [-f FREQ]\n"
        "                  [-m SPIMODE] [-u USERCODE] [-i IDCODE] [--background]\n"
        "                  [-D DBDIR]\n"
        "       zfpga synth IN.v [-l PINS.lpf] [-o OUT.zl] [-d DEVICE] [-p PACKAGE]\n"
        "       zfpga place IN.zl [-o OUT.zn] [-e EFFORT] [-s SEED] [-D DBDIR]\n"
        "       zfpga pnr IN.zn [-o OUT.cfg] [-D DBDIR]\n"
        "       zfpga unpack IN.bit [-o OUT.cfg] [-D DBDIR]\n"
        "       zfpga bram IN.{cfg,bit} -f FROM.hex -t TO.hex -o OUT [-D DBDIR]\n"
        "       zfpga bram -g OUT.hex -w WIDTH -d DEPTH [-s SEED]\n"
        "       zfpga jump TARGET [-b BOARD] [-o OUT.bit] [-D DBDIR]\n"
        "       zfpga flash FILE.bit [-a ADDR]     (on the machine)\n"
        "       zfpga run FILE.bit [-a ADDR]       (flash, then boot it)\n"
        "       zfpga info DEVICE [R<row>C<col>] [-D DBDIR]\n"
        "       zfpga version\n");
    zio_exit(2);
}

static const char *need(int argc, char **argv, int *i) {
    if (*i + 1 >= argc) {
        zf_note("%s needs a value", argv[*i]);
        usage();
    }
    return argv[++*i];
}

static uint32_t need_uint(int argc, char **argv, int *i) {
    const char *opt = argv[*i];
    const char *v = need(argc, argv, i);
    uint32_t n;
    if (!zf_parse_uint(v, &n)) zf_fatal("%s: '%s' is not a number", opt, v);
    return n;
}

static const char *dbdir_or_die(const char *d) {
    if (!d) zf_fatal("no database directory; give -D DIR");
    return d;
}

/* IN.config -> IN.bit; anything else gets .bit appended. */
static const char *default_out(const char *in) {
    size_t n = zf_strlen(in), dot = n, i;
    char *out;
    for (i = n; i > 0; i--) {
        if (in[i - 1] == '/') break;
        if (in[i - 1] == '.') { dot = i - 1; break; }
    }
    out = zf_alloc(dot + 5);
    zf_memcpy(out, in, dot);
    zf_memcpy(out + dot, ".bit", 5);
    return out;
}

int cmd_pack(int argc, char **argv) {
    const char *in = NULL, *out = NULL, *dbdir = DEFAULT_DB;
    zf_packopts_t o;
    zdb_t *db = zf_alloc(sizeof(*db));
    zf_chip_t *c = zf_alloc(sizeof(*c));
    int i;

    zf_memset(&o, 0, sizeof(o));
    o.multiboot = -1;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (zf_streq(a, "-o")) out = need(argc, argv, &i);
        else if (zf_streq(a, "-c")) o.compress = 1;
        else if (zf_streq(a, "-a")) { o.have_bootaddr = 1; o.bootaddr = need_uint(argc, argv, &i); }
        else if (zf_streq(a, "-J")) o.jump = 1;
        else if (zf_streq(a, "-f")) o.freq = need(argc, argv, &i);
        else if (zf_streq(a, "-m")) o.spimode = need(argc, argv, &i);
        else if (zf_streq(a, "-u")) { o.have_usercode = 1; o.usercode = need_uint(argc, argv, &i); }
        else if (zf_streq(a, "-i")) {
            o.have_idcode = 1;
            o.idcode = need_uint(argc, argv, &i);
            if (!o.idcode) zf_fatal("-i: idcode 0 is invalid");
        }
        else if (zf_streq(a, "--background")) o.background = 1;
        else if (zf_streq(a, "-D")) dbdir = need(argc, argv, &i);
        else if (a[0] == '-') { zf_note("unknown option %s", a); usage(); }
        else if (!in) in = a;
        else { zf_note("more than one input"); usage(); }
    }
    if (!in) usage();
    if (!out) out = default_out(in);

    cfg_load(c, db, dbdir_or_die(dbdir), in);
    pack_apply_options(c, &o);
    pack_write(c, &o, out);
    return 0;
}

int cmd_info(int argc, char **argv) {
    const char *dev = NULL, *dbdir = DEFAULT_DB, *where = NULL;
    zdb_t *db = zf_alloc(sizeof(*db));
    const zdb_hdr_t *h;
    int i;
    uint32_t k;

    for (i = 1; i < argc; i++) {
        if (zf_streq(argv[i], "-D")) dbdir = need(argc, argv, &i);
        else if (argv[i][0] == '-') usage();
        else if (!dev) dev = argv[i];
        else where = argv[i];
    }
    if (!dev) usage();

    /* zfpga info DEVICE R2C4: what is there, for writing loc= by hand */
    if (where) {
        char pre[24];
        size_t n;
        int found = 0;
        zdb_load(db, dbdir_or_die(dbdir), dev);
        zf_fmt(pre, sizeof(pre), "%s:", where);
        n = zf_strlen(pre);
        for (k = 0; k < db->n_tile; k++) {
            const char *tn = ZDB_STR(db, db->tile[k].name), *p = tn;
            const char *ty = ZDB_STR(db, db->type[db->tile[k].type].name);
            size_t j;
            /* the location part of the name, after any "CIB_" style prefix */
            while (*p && !(p[0] == 'R' && p[1] >= '0' && p[1] <= '9')) p++;
            for (j = 0; j < n && p[j] == pre[j]; j++) ;
            if (j != n) continue;
            if (zf_streq(ty, "PLC2"))
                zf_print("%-26s logic: loc=%s.A0 .. %s.D1\n", tn, where, where);
            else
                zf_print("%s\n", tn);
            found = 1;
        }
        for (k = 0; k < db->n_pio; k++) {
            char site[24];
            zf_fmt(site, sizeof(site), "R%uC%u", db->pio[k].row, db->pio[k].col);
            if (zf_streq(site, where))
                zf_print("PIO%c at %-19s bank %u\n", db->pio[k].letter, where, db->pio[k].bank);
        }
        if (!found) zf_print("nothing at %s\n", where);
        return 0;
    }

    zdb_load(db, dbdir_or_die(dbdir), dev);
    h = db->hdr;
    zf_print("device    %s (IDCODE 0x%08x)\n", db->device, db->idcode);
    zf_print("frames    %u x %u bits = %u bytes of CRAM\n", h->frames,
        h->bits_per_frame, (unsigned)h->frames * (((unsigned)h->bits_per_frame +
        h->pad_before + h->pad_after) / 8u));
    zf_print("tiles     %u of %u types\n", db->n_tile, h->sect[ZDB_S_TYPE].count);
    zf_print("arcs      %u   words %u   enums %u   bits %u\n",
        h->sect[ZDB_S_ARC].count, h->sect[ZDB_S_WORD].count,
        h->sect[ZDB_S_ENUM].count, h->sect[ZDB_S_BIT].count);
    zf_print("serves   ");
    for (k = 0; k < h->n_variants; k++)
        zf_print(" %s", ZDB_STR(db, db->var[k].name));
    zf_print("\n");
    zf_print("database  %u bytes, prjtrellis-db %s\n", db->size, ZDB_STR(db, h->source));
    return 0;
}

/* Split as the posix shell quoted it -- see zcc/main.c's split_args(). */
static char arg_buf[2 * 512 + 16];

static int split_args(char *line, char **av, int max) {
    int n = z_args_split(line, arg_buf, sizeof(arg_buf), av, NULL, max);
    for (int i = 0; i < n; i++) z_args_unmark(av[i]);
    return n;
}

int main(int argc, char **argv) {
    /* The DEVICE build must not look at the incoming argc/argv at all:
     * a Zeitlos process is started by name and a0/a1 hold whatever the
     * kernel left there. docs/zcc.md records the crash that taught this.
     * The command line comes from the launch argument instead, with the
     * program name prepended so that argv[0] means the same thing on
     * both builds. */
    static char argline[512];
    static char *dev_argv[33];
    int rc;

#if ZFPGA_HOSTED
    /* The host port never has a launch argument, but the path is kept
     * live so the tested build and the shipped one parse alike. */
    if (zio_get_args(argline, sizeof(argline))) {
        dev_argv[0] = "zfpga";
        argc = 1 + split_args(argline, dev_argv + 1, 32);
        argv = dev_argv;
    }
#else
    if (!zio_get_args(argline, sizeof(argline))) argline[0] = 0;
    dev_argv[0] = "zfpga";
    argc = 1 + split_args(argline, dev_argv + 1, 32);
    argv = dev_argv;
#endif

    zio_out_open();

    if (argc < 2) usage();

    /* The commands that load the database need most of the 4MB tier the
     * kernel gives zfpga by name (sw/os/kernel.h). A kernel without that
     * line gives the default 16KB, and every one of them would fail at
     * its first allocation; say so plainly, before anything else. */
    if (zf_streq(argv[1], "build") || zf_streq(argv[1], "place") || zf_streq(argv[1], "pnr") ||
            zf_streq(argv[1], "pack") || zf_streq(argv[1], "unpack") || zf_streq(argv[1], "bram") || zf_streq(argv[1], "jump")) {
        void *probe = zio_block(3328u * 1024u);
        if (!probe)
            zf_fatal("this process has less than 3.3MB of memory; zfpga needs the 4MB tier. "
                "The running kernel does not list zfpga in z_proc_stack_size_for() "
                "(sw/os/kernel.h): rebuild and reflash Zeitlos -- docs/zfpga-test.md sec. 1");
        zio_free(probe);
    }
    if (zf_streq(argv[1], "pack")) rc = cmd_pack(argc - 1, argv + 1);
    else if (zf_streq(argv[1], "info")) rc = cmd_info(argc - 1, argv + 1);
    else if (zf_streq(argv[1], "pnr")) rc = cmd_pnr(argc - 1, argv + 1, DEFAULT_DB);
    else if (zf_streq(argv[1], "place")) rc = cmd_place(argc - 1, argv + 1, DEFAULT_DB);
    else if (zf_streq(argv[1], "unpack")) rc = cmd_unpack(argc - 1, argv + 1, DEFAULT_DB);
    else if (zf_streq(argv[1], "synth")) rc = cmd_synth(argc - 1, argv + 1);
    else if (zf_streq(argv[1], "build")) rc = cmd_build(argc - 1, argv + 1, DEFAULT_DB);
    else if (zf_streq(argv[1], "bram")) rc = cmd_bram(argc - 1, argv + 1, DEFAULT_DB);
    else if (zf_streq(argv[1], "jump")) rc = cmd_jump(argc - 1, argv + 1, DEFAULT_DB);
    else if (zf_streq(argv[1], "flash")) rc = cmd_flash(argc - 1, argv + 1);
    else if (zf_streq(argv[1], "run")) rc = cmd_run(argc - 1, argv + 1);
    else if (zf_streq(argv[1], "version")) { zf_print("zfpga %s\n", ZFPGA_VERSION); rc = 0; }
    else { zf_note("unknown command '%s'", argv[1]); usage(); }

    zio_exit(rc);
    return rc;
}
