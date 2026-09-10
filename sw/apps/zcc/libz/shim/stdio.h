/* Freestanding <stdio.h> for building sw/common sources into libz.
 *
 * Declares only what those sources actually call, with signatures
 * matching libz's own -- deliberately NOT "#include libz.h", which
 * would redeclare maskirq() and z_msg_send() alongside zeitlos.h's
 * versions of the same names and collide. The shim's whole job is to
 * be the smallest thing that satisfies the #include. */
#ifndef _STDIO_H
#define _STDIO_H
int printf(const char *fmt, ...);
int snprintf(char *buf, unsigned cap, const char *fmt, ...);
int puts(const char *s);
int putchar(int c);
int getchar(void);
#ifndef NULL
#define NULL ((void *)0)
#endif
#endif
