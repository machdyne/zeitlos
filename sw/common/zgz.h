#ifndef ZGZ_H
#define ZGZ_H
/*
 * Zeitlos -- reading a .gz file: sw/common/zinflate.c behind the app
 * file API. `zcat` and `gunzip` in posix use it; so can any app.
 *
 *   static uint8_t window[Z_INFLATE_WINDOW];
 *   static z_gz_t g;
 *   if (z_gz_open(&g, "/notes.gz", window) == 0) {
 *       int32_t n;
 *       while ((n = z_gz_read(&g, buf, sizeof(buf))) > 0) use(buf, n);
 *       if (n < 0) report(z_gz_strerror(n));
 *       z_gz_close(&g);
 *   }
 *
 * The 32 KB window is the caller's, as zinflate.h explains: an app that
 * never decompresses anything does not pay for it. The file is read a
 * kilobyte at a time; nothing else is buffered.
 *
 * gzip's optional header fields are accepted (gzip_fields): a file
 * made by `gzip file` always carries its name. The trailer IS checked
 * here -- the CRC-32 of every byte returned, and the length -- though
 * zinflate does not check it for the web client, which cannot spare
 * the time; a corrupt .gz on a card should say so, not decode to
 * something else. A file that ends before its trailer is reported as
 * truncated, never as a clean end.
 */
#include <stdint.h>
#include <stdbool.h>
#include "zinflate.h"

#define Z_GZ_E_TRUNCATED  -10   // the file ended before the stream did
#define Z_GZ_E_READ       -11   // the file could not be read
#define Z_GZ_E_CRC        -12   // the data does not match the trailer

typedef struct {
	int handle;
	z_inflate_t z;
	uint8_t in[1024];
	uint32_t in_len, in_pos;
	bool eof;               // the file has nothing more
	int32_t rv;             // the last result: 0 reading, 1 done, < 0 an error
	uint32_t crc, size;     // of the bytes returned so far
} z_gz_t;

// 0, or -1 if the file cannot be opened.
int z_gz_open(z_gz_t *g, const char *path, uint8_t *window);

// Up to n bytes: > 0 read; 0 the stream ended cleanly (trailer checked);
// < 0 Z_INFLATE_E_* or Z_GZ_E_*. Bytes decoded before an error are
// returned first; the error comes on the next call.
int32_t z_gz_read(z_gz_t *g, void *buf, uint32_t n);

void z_gz_close(z_gz_t *g);
const char *z_gz_strerror(int32_t rv);

#endif
