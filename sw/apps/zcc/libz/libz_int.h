/* libz internals -- not part of the app-facing header. */
#ifndef LIBZ_INT_H
#define LIBZ_INT_H

void libz_finish(void);
extern unsigned libz_heap_start;    /* patched by zcc: the image's _end */

#endif
