/*
 * zcc -- the seam between the host build and the device build.
 *
 * The compiler's own code is the same in both. What differs is four
 * things, and only four: how a file is read, how a file is written,
 * how a diagnostic reaches a screen, and how the process stops. Those
 * are behind zio_*, implemented by port_host.c against stdio and by
 * port_dev.c against libz and the Zeitlos filesystem syscalls.
 *
 * -- Why a seam rather than #ifdefs in place --
 *
 * Because the device build is the one that matters and the host build
 * is the one that gets tested. Scattering `#ifdef __zeitlos__` through
 * the lexer and the emitter would mean the tested code and the shipped
 * code diverge line by line, in the files where a difference is
 * hardest to see. Four functions in two files is a difference you can
 * read in one sitting.
 *
 * -- Whole files, not streams --
 *
 * zio_read_file() returns the entire file in one allocation and
 * zio_write_file() takes the entire output in one call. That is not
 * laziness about buffering; it is what the target wants. FatFs is
 * non-reentrant and the syscall dispatcher refuses to preempt a
 * process inside it (docs/filesystem.md), so a compiler that dribbled
 * through a file in small reads would enter and leave that critical
 * region hundreds of times per source file. One read, one write.
 *
 * It also matches what the compiler already did: the lexer reads a
 * whole file up front because it needs to look ahead freely, and the
 * emitter cannot write anything until every fixup is resolved.
 */

#ifndef ZCC_PORT_H
#define ZCC_PORT_H

/*
 * ZCC_HOSTED is 1 for the development-host build and 0 for the
 * Zeitlos one. Nothing outside port_*.c should test it -- if a third
 * place needs to know, that is a sign something belongs behind zio_*.
 */
#ifndef ZCC_HOSTED
#define ZCC_HOSTED 1
#endif

/*
 * Reads a whole file. Returns a NUL-terminated buffer (one byte longer
 * than the file) or NULL, and sets *len to the file's length.
 *
 * The extra NUL is what lets the lexer treat the buffer as a string
 * and stop at the end without a separate bounds test on every
 * character.
 */
char *zio_read_file(const char *path, int *len);

/* Writes a whole file. Returns 0 on success. */
int zio_write_file(const char *path, const void *buf, int len);

/* Diagnostics and any other output, already formatted. */
void zio_out(const char *s);

/*
 * Opens and closes the output channel.
 *
 * On the host these do nothing. On the device they are what makes
 * `zcc hello.c` at a `posix` prompt print into the term window rather
 * than onto the serial console: zcc is a separate process, so its
 * output has no connection to the shell's unless it asks for one.
 *
 * zio_out_open() must be called before any output and zio_out_close()
 * after the last -- including on the error path, since a compiler's
 * whole purpose on a bad day is the message it prints before exiting.
 * zio_exit() calls it, so nothing else has to remember.
 */
void zio_out_open(void);
void zio_out_close(void);

/* Does not return. */
void zio_exit(int status);

/*
 * Where a command line comes from on a machine that has no argv.
 *
 * Returns 1 having filled `buf`, or 0 to mean "use the argc/argv main
 * was called with". The host build always returns 0.
 *
 * A Zeitlos process is started by name and nothing else: `run zcc` at
 * the shell, or Z_SYS_PROC_RUN from the dock, neither of which carries
 * arguments (sw/os/kernel.c, k_proc_run). So the device build looks in
 * two places, in order:
 *
 *   1. the launch argument (z_launch_arg_take, sw/common/zwin.h),
 *      which is the mechanism wm already uses to hand a filename to an
 *      app it launches;
 *   2. the file /zcc.args, one command line.
 *
 * The second is a STOPGAP and should be read as one. It exists so that
 * the compiler is usable and testable before there is anything that
 * can pass it a real argv -- which is precisely what `posix` is for
 * (docs/posix.md, Phase 4). When that lands, the shell sets the launch
 * argument and this file stops being interesting.
 */
int zio_get_args(char *buf, int cap);

/*
 * ctype, ours, in both builds.
 *
 * Not because libz lacks them -- it could have them -- but because the
 * host's are locale-dependent by specification and the compiler's
 * lexer must not be. `isalpha` returning true for a byte above 127 in
 * some locale would silently change which identifiers are legal,
 * depending on the environment the compiler was run from. These are
 * ASCII and nothing else, in both builds, so a source file lexes the
 * same way everywhere.
 *
 * They take int rather than char and are written to be safe for the
 * negative values a signed char produces -- the classic ctype trap,
 * and one that a compiler feeds a lot of bytes through.
 */
static inline int z_isdigit(int c) { return c >= '0' && c <= '9'; }
static inline int z_isupper(int c) { return c >= 'A' && c <= 'Z'; }
static inline int z_islower(int c) { return c >= 'a' && c <= 'z'; }
static inline int z_isalpha(int c) { return z_isupper(c) || z_islower(c); }
static inline int z_isalnum(int c) { return z_isalpha(c) || z_isdigit(c); }
static inline int z_isxdigit(int c) {
    return z_isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static inline int z_isspace(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == '\v' || c == '\f';
}
static inline int z_tolower(int c) { return z_isupper(c) ? c + 32 : c; }

#endif
