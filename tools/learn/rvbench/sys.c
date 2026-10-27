/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * tools/learn/rvbench -- the C library's system calls, for sim.py
 */
#include <stdint.h>
#include <sys/stat.h>
#include <errno.h>
static inline long ec(long n, long a, long b, long c, long d) {
	register long a0 asm("a0") = a, a1 asm("a1") = b, a2 asm("a2") = c, a3 asm("a3") = d, a7 asm("a7") = n;
	asm volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a3), "r"(a7) : "memory");
	return a0;
}
long sim_call(long n, long a, long b, long c, long d) { return ec(n, a, b, c, d); }
int _write(int fd, const void *b, int n) { return ec(64, fd, (long)b, n, 0); }
int _read(int fd, void *b, int n) { (void)fd; (void)b; (void)n; return 0; }
extern char _end[];
static char *brk_ = _end;
void *_sbrk(int inc) { char *o = brk_; brk_ += inc; return o; }
void _exit(int c) { ec(93, c, 0, 0, 0); for (;;); }
int _close(int f) { (void)f; return -1; }
int _lseek(int f, int o, int w) { (void)f; (void)o; (void)w; return 0; }
int _fstat(int f, struct stat *s) { (void)f; s->st_mode = S_IFCHR; return 0; }
int _isatty(int f) { (void)f; return 1; }
int _kill(int p, int s) { (void)p; (void)s; errno = EINVAL; return -1; }
int _getpid(void) { return 1; }
int _open(const char *p, int f, int m) { (void)p; (void)f; (void)m; return -1; }
int _gettimeofday(void *t, void *z) { (void)t; (void)z; return -1; }
