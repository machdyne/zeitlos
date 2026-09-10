#ifndef _STDDEF_H
#define _STDDEF_H

typedef unsigned int size_t;
typedef int ptrdiff_t;

#ifndef NULL
#define NULL ((void *)0)
#endif

/* offsetof through a null pointer is undefined in principle and is the
 * only definition available without a compiler builtin. It works here
 * because nothing dereferences the result -- and because zcc computes
 * a member address as base + constant with no null check. */
#define offsetof(t, m) ((size_t)&(((t *)0)->m))

#endif
