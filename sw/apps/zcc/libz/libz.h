/*
 * libz -- the runtime a zcc-compiled program links against.
 *
 * This is the header a program includes. Nothing in it is a macro over
 * something else: every name here is a real function living in the
 * runtime blob, reached through a jump table slot that zcc resolves by
 * index. See docs/libz.md.
 *
 * -- What this is not --
 *
 * It is not newlib, and it is not trying to be. docs/app_runtime.md
 * records what newlib costs on this machine: about 100KB for the
 * printf formatter, and about 40KB more for anything touching a FILE,
 * to the point that the tree's own advice is to format numbers by hand
 * rather than call printf. Handing that bill to zcc output -- already
 * four and a half times GCC's size -- would produce a compiler whose
 * hello-world does not fit.
 *
 * So this is a small, purpose-built runtime: the subset the tree
 * actually uses, sized for the machine it runs on. printf here handles
 * %d %u %x %c %s %p with width, precision and zero-padding, and costs
 * about 1KB.
 */

#ifndef LIBZ_H
#define LIBZ_H

/*
 * Bumped whenever libz_table.def changes.
 *
 * Stamped into the blob and written into libz.syms; zcc compares the
 * two and refuses a mismatch. This is what stops a binary built
 * against one table being run against another -- see libz_table.def's
 * own header for why that failure is silent otherwise.
 */
#define LIBZ_ABI_VERSION 2

/* start.S includes this header for LIBZ_ABI_VERSION alone, so the
 * declarations below have to be invisible to the assembler. The
 * alternative -- a second header holding just the version -- is a
 * second place for it to be wrong. */
#ifndef __ASSEMBLER__

typedef unsigned int size_t;
typedef int ssize_t;

#ifndef NULL
#define NULL ((void *)0)
#endif

/* -- memory -- */
void *malloc(size_t n);
void free(void *p);
void *calloc(size_t n, size_t sz);
void *realloc(void *p, size_t n);
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
void *memchr(const void *s, int c, size_t n);

/* -- strings -- */
size_t strlen(const char *s);
char *strcpy(char *d, const char *s);
char *strncpy(char *d, const char *s, size_t n);
char *strcat(char *d, const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *h, const char *n);
int atoi(const char *s);

/* -- console -- */
int putchar(int c);
int puts(const char *s);
/* puts() appends a newline, which is right most of the time and wrong
 * when building a line piecewise. fputs_stdout() is the one that does
 * not -- named for what it does rather than taking a FILE this runtime
 * does not have. */
int fputs_stdout(const char *s);
int getchar(void);
int kbhit(void);
int printf(const char *fmt, ...);
int snprintf(char *buf, size_t cap, const char *fmt, ...);
/* `ap` is a va_list. Declared void * because zcc has no <stdarg.h> and
 * this header must be readable by it -- and because on rv32 a va_list
 * IS a pointer, so the two are the same object. A zcc-compiled program
 * cannot construct one and has no reason to call this; it is here for
 * GCC-built callers and for snprintf's own use. */
int vsnprintf(char *buf, size_t cap, const char *fmt, void *ap);

/* -- process, messaging, filesystem and hardware --
 *
 * Declared by sw/common's own headers, included below, with
 * sw/common's own types. NOT redeclared here: a second declaration,
 * however carefully matched, is a second place for the two to
 * disagree, and the compiler's error when they do points at this file
 * rather than at the mismatch. That happened, with `unsigned` against
 * `uint32_t`, which are the same type right up until the header search
 * path changes.
 *
 * The runtime provides the tree's OWN names -- fs_mallocfile(), not a
 * parallel fs_read() -- so a program moving between a GCC build and a
 * zcc build does not change a single call. Which is the point of
 * resolving names at declaration time: no special header, no
 * annotation, no second API.
 */
#include "zeitlos.h"
#include "zfsapp.h"

/* -- the runtime's own --
 *
 * These three are libz's, not sw/common's, so they are declared here.
 * z_exit() in particular has no counterpart in zeitlos.h: an app built
 * with newlib returns from main() and crt0 handles it, whereas libz
 * has no crt0 and the entry stub calls this instead. */
void z_exit(int status);
void *z_syscall(unsigned id, void *arg);
size_t z_heap_used(void);

#endif /* __ASSEMBLER__ */

#endif
