/*
 * zcc -- driver.
 *
 * Usage mirrors what a C compiler user already expects, minus
 * everything this one does not have: there is no -c, because there are
 * no object files; no -l, because there is no linker; and no -O,
 * because there is one code generator and it always does the same
 * thing. What is left is -o, -I, -D and a source file.
 *
 * -E exists and is worth keeping even though nothing needs it to
 * build: a preprocessor bug and a parser bug look identical from the
 * outside, and being able to see the token stream is the difference
 * between five minutes and an afternoon.
 */

#include <string.h>
#include <stdint.h>

#include "zcc.h"

/* The libz ABI this compiler was built to speak. Must match
 * LIBZ_ABI_VERSION in libz/libz.h; the two are checked against each
 * other every time a blob is loaded. */
#define LIBZ_EXPECTED_ABI 3

/*
 * Loads libz.bin and libz.sym from a directory.
 *
 * The .syms format is one directive per line with space-separated
 * fields -- see libz/mksyms.py. Deliberately trivial to parse: this
 * exact code has to run on the device in Phase 3, with no line-
 * splitting library and a compiler that has no sscanf.
 */
typedef struct {
    int have;
    uint8_t *blob;
    int blob_len;
    int table_off;
    int patch_main, patch_heap;
    int version;
    libz_sym_t *syms;
    int nsyms;
} libz_t;

static int read_word(const char **p, char *out, int cap) {
    const char *s = *p;
    int n = 0;
    while (*s == ' ' || *s == '\t') s++;
    while (*s && *s != ' ' && *s != '\t' && *s != '\n') {
        if (n < cap - 1) out[n++] = *s;
        s++;
    }
    out[n] = 0;
    *p = s;
    return n;
}

static int parse_int(const char *s) {
    int v = 0, neg = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

static int libz_load(libz_t *lz, const char *dir) {

    char path[512];
    char line[512];

    zcc_snprintf(path, sizeof(path), "%s/libz.sym", dir);
    char *syms = zio_read_file(path, NULL);
    if (!syms) return 0;

    lz->table_off = -1;
    lz->patch_main = -1;
    lz->patch_heap = -1;

    int cap = 64;
    lz->syms = zalloc(sizeof(libz_sym_t) * (size_t)cap);

    /* Walked line by line over the whole file rather than read through
     * fgets(): the device build has no stdio at all, and a format this
     * simple does not need one. Same code both sides. */
    for (const char *p = syms; *p; ) {
        int n = 0;
        while (p[n] && p[n] != '\n') n++;
        if (n >= (int)sizeof(line)) n = (int)sizeof(line) - 1;
        memcpy(line, p, (size_t)n);
        line[n] = 0;
        p += n;
        if (*p == '\n') p++;

        const char *q = line;
        char kind[64], a[64], b[256];

        if (!read_word(&q, kind, sizeof(kind))) continue;
        if (kind[0] == '#') continue;

        if (!strcmp(kind, "version")) {
            read_word(&q, a, sizeof(a));
            lz->version = parse_int(a);
        } else if (!strcmp(kind, "blob")) {
            read_word(&q, a, sizeof(a));
            lz->blob_len = parse_int(a);
        } else if (!strcmp(kind, "table")) {
            read_word(&q, a, sizeof(a));
            lz->table_off = parse_int(a);
        } else if (!strcmp(kind, "patch")) {
            read_word(&q, a, sizeof(a));
            read_word(&q, b, sizeof(b));
            if (!strcmp(a, "main")) lz->patch_main = parse_int(b);
            else if (!strcmp(a, "heap")) lz->patch_heap = parse_int(b);
        } else if (!strcmp(kind, "sym")) {
            read_word(&q, a, sizeof(a));
            read_word(&q, b, sizeof(b));
            if (lz->nsyms == cap) {
                cap *= 2;
                libz_sym_t *nn = zalloc(sizeof(libz_sym_t) * (size_t)cap);
                memcpy(nn, lz->syms, sizeof(libz_sym_t) * (size_t)lz->nsyms);
                lz->syms = nn;
            }
            lz->syms[lz->nsyms].name = zstrdup(b);
            lz->syms[lz->nsyms].label = parse_int(a);   /* index for now */
            lz->nsyms++;
        }
    }

    if (lz->table_off < 0 || lz->patch_main < 0 || lz->patch_heap < 0)
        zcc_fatal("%s is missing a table or patch directive", path);

    if (lz->version != LIBZ_EXPECTED_ABI) {
        /* Refused, not warned about. A table whose order has changed
         * makes every call go to the wrong function, silently -- see
         * libz/libz_table.def's own header. */
        zcc_fatal("%s is libz ABI %d, this zcc expects %d",
                  path, lz->version, LIBZ_EXPECTED_ABI);
    }

    zcc_snprintf(path, sizeof(path), "%s/libz.bin", dir);
    int n = 0;
    lz->blob = (uint8_t *)zio_read_file(path, &n);
    if (!lz->blob)
        zcc_fatal("found libz.sym but not %s", path);

    if (n != lz->blob_len)
        zcc_fatal("%s is %d bytes, libz.sym says %d -- rebuild libz",
                  path, n, lz->blob_len);

    lz->have = 1;
    return 1;
}

static void usage(const char *argv0) {
    zcc_printf(
        "usage: %s [options] <input.c>\n"
        "\n"
        "  -o FILE     output image (default a.bin)\n"
        "  -I DIR      add an include search directory\n"
        "  -D NAME[=V] define a macro\n"
        "  -U NAME     undefine a macro\n"
        "  -L DIR      look for the libz runtime here\n"
        "  -nolibz     freestanding: no runtime, zcc emits its own entry stub\n"
        "  -E          preprocess only, print the token stream\n"
        "  -v          report image sizes on stderr\n"
        "\n"
        "Produces a ZEXE image (docs/executables.md) linked at\n"
        "0x80000000, ready to run. There is no separate assembly or\n"
        "link step -- see docs/zcc.md.\n",
        argv0);
}

static void dump_tokens(token_t *t) {
    int line = -1;
    for (; t && t->kind != TK_EOF; t = t->next) {
        if (t->line != line) { zio_out("\n"); line = t->line; }
        else if (t->has_space) zio_out(" ");
        if (t->kind == TK_STR) zcc_printf("\"%s\"", t->str);
        else if (t->kind == TK_NUM && (!t->text || !*t->text)) zcc_printf("%u", t->val);
        else zio_out(t->text);
    }
    zio_out("\n");
}

/*
 * Splits a command line in place. No quoting and no escapes: a Zeitlos
 * path has no spaces in it, and the alternative is a shell, which is
 * `posix`'s job and not this file's.
 */
static int split_args(char *line, char **argv, int max) {
    int n = 1;
    argv[0] = "zcc";
    for (char *p = line; *p && n < max; ) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[n++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = 0;
    }
    return n;
}

int main(int argc, char **argv) {

    /*
     * On a machine with no argv, the command line comes from the
     * launch argument or from a file -- see zio_get_args().
     *
     * The DEVICE BUILD MUST NOT LOOK AT THE INCOMING argc/argv AT ALL,
     * and this is worth spelling out because getting it wrong crashed
     * a real machine.
     *
     * zcc_start.S calls main() without setting a0/a1 -- there is
     * nothing to set them to, since a Zeitlos process is started by
     * name and carries no arguments. So on entry they hold whatever
     * the kernel happened to leave in those registers. The first
     * version of this took the `zio_get_args() returned 0` path and
     * carried on with that garbage: `zcc` with no arguments walked a
     * random argc, dereferenced random pointers, and with no MMU a
     * wild pointer below 0x8000_0000 reaches the kernel and the
     * peripherals directly. It did not fault, it took the system down.
     *
     * The simulator could not reproduce it: cpu_reset() zeroes the
     * register file, so a0/a1 were 0 and the loop simply did not run.
     * sim/ now poisons the registers instead, for exactly this reason.
     */
    static char argline[512];
    static char *dev_argv[32];

#if ZCC_HOSTED
    if (zio_get_args(argline, sizeof(argline))) {
        argc = split_args(argline, dev_argv, 32);
        argv = dev_argv;
    }
#else
    if (!zio_get_args(argline, sizeof(argline))) argline[0] = 0;
    argc = split_args(argline, dev_argv, 32);
    argv = dev_argv;
#endif

    const char *out = "a.bin";
    const char *in = NULL;
    const char *libdirs[8];
    int nlibdirs = 0;
    int preprocess_only = 0;
    int nolibz = 0;
    libz_t lz;

    memset(&lz, 0, sizeof(lz));

    for (int i = 1; i < argc; i++) {
        char *a = argv[i];

        if (!strcmp(a, "-o")) {
            if (++i >= argc) { usage(argv[0]); return 1; }
            out = argv[i];
        } else if (!strncmp(a, "-I", 2)) {
            if (a[2]) cpp_add_include_dir(a + 2);
            else {
                if (++i >= argc) { usage(argv[0]); return 1; }
                cpp_add_include_dir(argv[i]);
            }
        } else if (!strncmp(a, "-D", 2)) {
            if (a[2]) cpp_define_cli(a + 2);
            else {
                if (++i >= argc) { usage(argv[0]); return 1; }
                cpp_define_cli(argv[i]);
            }
        } else if (!strncmp(a, "-U", 2)) {
            if (a[2]) cpp_undef_cli(a + 2);
            else {
                if (++i >= argc) { usage(argv[0]); return 1; }
                cpp_undef_cli(argv[i]);
            }
        } else if (!strncmp(a, "-L", 2)) {
            const char *d;
            if (a[2]) d = a + 2;
            else {
                if (++i >= argc) { usage(argv[0]); return 1; }
                d = argv[i];
            }
            if (nlibdirs < 8) libdirs[nlibdirs++] = d;
        } else if (!strcmp(a, "-nolibz")) {
            nolibz = 1;
        } else if (!strcmp(a, "-E")) {
            preprocess_only = 1;
        } else if (!strcmp(a, "-v")) {
            zcc_verbose = 1;
        } else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage(argv[0]);
            return 0;
        } else if (a[0] == '-' && a[1]) {
            zcc_printf("zcc: unknown option '%s'\n", a);
            return 1;
        } else {
            in = a;
        }
    }

    if (!in) { zio_out_open(); usage(argv[0]); zio_out_close(); return 1; }

    /* Two predefined macros, and only two.
     *
     * __zcc__ so a header can tell which compiler it is being read by,
     * and __riscv so the tree's existing arch tests work unchanged.
     * Not __STDC__, because claiming conformance this compiler does
     * not have would make a header take a path it cannot compile --
     * a false claim is worse than a missing one. */
    /* Predefines happen here, after the argument loop, so that a -U on
     * the command line can remove one. Doing them first and letting -D
     * override was the other option; -U-after is what a caller
     * actually reaches for when a header takes a path this compiler
     * cannot follow. */
    cpp_define_cli("__zcc__=1");
    cpp_define_cli("__riscv=1");
    cpp_define_cli("__riscv_xlen=32");
    for (int i = 1; i < argc; i++)
        if (!strncmp(argv[i], "-U", 2) && argv[i][2])
            cpp_undef_cli(argv[i] + 2);

    if (!nolibz)
        for (int i = 0; i < nlibdirs && !lz.have; i++)
            libz_load(&lz, libdirs[i]);

    /* -L given and nothing found is an ERROR, not a silent fallback to
     * a freestanding build. The freestanding build has no printf and
     * no malloc, so the fallback surfaces much later as every runtime
     * name being undefined -- a page of errors whose actual cause is a
     * mistyped path. Asking for a runtime and not getting one should
     * say so where it happened. */
    if (!nolibz && nlibdirs && !lz.have)
        zcc_fatal("no libz.sym in any -L directory (looked in %s%s) -- "
                  "build it with 'make -C libz', or pass -nolibz for a "
                  "freestanding build",
                  libdirs[0], nlibdirs > 1 ? ", ..." : "");

    /* Before anything is printed, and after the arguments are known --
     * see zio_out_open(). On the device this connects back to `posix`
     * if one is running, so that output lands in the terminal the
     * command was typed in rather than on the serial console. */
    zio_out_open();

    token_t *tok = lex_file(in);
    if (!tok) {
        zcc_printf("zcc: cannot read '%s'\n", in);
        return 1;
    }

    tok = preprocess(tok);

    if (preprocess_only) {
        dump_tokens(tok);
        zio_out_close();
        return 0;
    }

    /* The blob goes down first, at .text offset 0, so that the
     * addresses it was linked against are the addresses it lands at.
     * Then each table slot becomes a pinned label, so a declaration of
     * one of its names resolves to a single `jal` into the table. */
    if (lz.have) {
        emit_prepend_blob(lz.blob, lz.blob_len);
        for (int i = 0; i < lz.nsyms; i++)
            lz.syms[i].label = emit_pin_text_label(lz.table_off + 4 * i);
        parse_set_libz(lz.syms, lz.nsyms);
        if (zcc_verbose)
            zcc_printf("zcc: libz abi %d, %d entries, %d byte blob\n",
                       lz.version, lz.nsyms, lz.blob_len);
    }

    parse_translation_unit_ex(tok,
        lz.have ? lz.patch_main : -1,
        lz.have ? lz.patch_heap : -1);

    if (emit_write_zexe(out, parse_entry_label(), zcc_verbose) != 0) {
        zio_out_close();
        return 1;
    }

    if (zcc_verbose)
        zcc_printf("zcc: %u KB of arena used\n",
                   (unsigned)((zalloc_total() + 1023) / 1024));

    zio_out_close();

    return 0;
}
