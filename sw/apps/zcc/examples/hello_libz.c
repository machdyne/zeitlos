/*
 * hello_libz.c -- the second test: the same idea, using the runtime.
 *
 *     zcc -I /libz -I /common -I /include -L /libz -o /hellz /hello_libz.c
 *     run hellz
 *
 * This one needs more on the card: libz.bin and libz.sym for -L, and
 * the headers libz.h pulls in (zeitlos.h, zfsapp.h from /common;
 * stdint.h and friends from /include).
 *
 * What it proves over hello.c is the whole runtime path -- the blob
 * being embedded, the jump table being resolved, printf and malloc
 * and the filesystem all reached through it. If hello.c works and
 * this does not, the problem is libz or the files it needs, not the
 * compiler.
 */

#include "libz.h"

int main(void) {
    char *buf;
    int i;

    printf("hello from zcc + libz\n");

    buf = malloc(64);
    if (!buf) {
        printf("malloc failed -- no heap?\n");
        return 1;
    }

    strcpy(buf, "the runtime works");
    printf("%s (%u chars)\n", buf, (unsigned)strlen(buf));
    free(buf);

    for (i = 1; i <= 8; i++)
        printf("  %d squared is %3d\n", i, i * i);

    printf("uptime %u ticks, pid %u\n",
           (unsigned)z_uptime_ticks(), (unsigned)z_getpid());

    return 0;
}
