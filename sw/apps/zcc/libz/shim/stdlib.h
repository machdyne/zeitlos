#ifndef _STDLIB_H
#define _STDLIB_H
void *malloc(unsigned n);
void free(void *p);
void *calloc(unsigned n, unsigned sz);
void *realloc(void *p, unsigned n);
int atoi(const char *s);
#ifndef NULL
#define NULL ((void *)0)
#endif
#endif
