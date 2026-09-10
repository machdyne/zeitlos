/*
 * Minimal Zeitlos syscall access for the zcc test suite.
 *
 * This is NOT the runtime -- that is Phase 2 (docs/posix.md, "libz").
 * It exists so Phase 1's tests can print something, and it is written
 * in plain C on purpose: every line of it exercises a feature the
 * compiler has to get right (a volatile pointer cast, a function
 * pointer cast from an integer, an indirect call, a struct on the
 * stack), so if it works the compiler is doing real work rather than
 * special-casing a builtin.
 */

#ifndef ZSYS_H
#define ZSYS_H

typedef struct { int type; unsigned val; } zobj;
typedef zobj *(*zkfn)(unsigned, void *, unsigned);

#define ZSYS_EXIT       1
#define ZSYS_UART_PUTC  4

static zobj *zsys(unsigned id, void *arg) {
    /* reg_kernel lives at 0x0000000c -- sw/common/zeitlos.h */
    unsigned k = *(volatile unsigned *)12;
    return ((zkfn)k)(id, arg, 0);
}

static void putch(int c) {
    zobj o;
    o.type = 3;             /* Z_INT32 */
    o.val = (unsigned)c;
    zsys(ZSYS_UART_PUTC, &o);
}

static void puts_(const char *s) {
    while (*s) putch(*s++);
}

static void putd(int v) {
    char b[12];
    int n = 0;
    unsigned u;
    if (v < 0) { putch('-'); u = (unsigned)(-v); } else u = (unsigned)v;
    if (!u) { putch('0'); return; }
    while (u) { b[n++] = (char)('0' + u % 10); u /= 10; }
    while (n) putch(b[--n]);
}

static void putu(unsigned u) {
    char b[12];
    int n = 0;
    if (!u) { putch('0'); return; }
    while (u) { b[n++] = (char)('0' + u % 10); u /= 10; }
    while (n) putch(b[--n]);
}

static void line(const char *tag, int v) {
    puts_(tag);
    puts_(" ");
    putd(v);
    putch('\n');
}

#endif
