/*
 * hello.c -- the smallest thing zcc can compile on the machine.
 *
 *     zcc -nolibz -o /hello /hello.c
 *     run hello
 *
 * NO #include, so this file is the ONLY thing that has to be on the
 * card. Not libz.bin, not /include, not /common. `-nolibz` tells zcc
 * to emit its own entry stub instead of embedding the runtime.
 *
 * That makes it the right FIRST test: if this works, the compiler,
 * the ZEXE image and the loader are all sound, and anything that fails
 * afterwards is the runtime or the shell rather than any of those.
 *
 * Output goes to the KERNEL CONSOLE (the UART), not to the `term`
 * window you typed the command in -- see docs/posix.md on why a
 * spawned program's output does not come back to the shell yet.
 */

typedef struct { int type; unsigned val; } zobj;
typedef zobj *(*kfn)(unsigned, void *, unsigned);

/* The kernel installs a function pointer at address 0x0c before a
 * process starts (docs/app_runtime.md, "The syscall trampoline"). A
 * syscall is an ordinary call through it -- no trap, no ecall. */
#define Z_KERNEL     (*(volatile unsigned *)12)
#define Z_UART_PUTC  4
#define Z_INT32      3

static void putch(int c) {
    zobj o;
    o.type = Z_INT32;
    o.val = (unsigned)c;
    ((kfn)Z_KERNEL)(Z_UART_PUTC, &o, 0);
}

static void print(const char *s) {
    while (*s) putch(*s++);
}

static void print_num(int v) {
    char buf[12];
    int n = 0;
    if (v < 0) { putch('-'); v = -v; }
    if (!v) { putch('0'); return; }
    while (v) { buf[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) putch(buf[--n]);
}

int main(void) {
    int i;

    print("hello from zcc\r\n");

    for (i = 1; i <= 8; i++) {
        print("  ");
        print_num(i);
        print(" squared is ");
        print_num(i * i);
        print("\r\n");
    }

    return 0;
}
