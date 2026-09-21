/*
 * zfpga -- the host/device seam.
 *
 * Everything that differs between the host build (Makefile.host, stdio)
 * and the device build (Makefile, the Zeitlos filesystem syscalls) is
 * behind these functions, in port_host.c and port_dev.c. Nothing else
 * in zfpga has an #ifdef for it.
 *
 * This is zcc's arrangement (docs/zcc.md, "The seam") with one change:
 * files are CHUNKED rather than read and written whole. zcc can hold a
 * translation unit; zfpga reads the SOC's .config, which runs to
 * megabytes, and writes a bitstream of up to 2MB. Chunks are also what
 * docs/posix.md sec. 2.5 asks for on this machine, because a whole-file
 * read inside FatFs is not preempted and blocks the window manager for
 * the duration.
 *
 * Why a seam rather than #ifdefs through the code: the device build is
 * the one that matters and the host build is the one that gets tested.
 * Scattered conditionals make those two diverge line by line.
 */

#ifndef ZFPGA_PORT_H
#define ZFPGA_PORT_H

#include <stddef.h>
#include <stdint.h>

typedef struct zio_file zio_file_t;

/* NULL if the file does not exist. An empty file is a valid handle --
 * docs/zcc.md, "The bug that only the device could show", is why that
 * distinction is made by opening rather than by asking for a size. */
zio_file_t *zio_open_read(const char *path);
zio_file_t *zio_open_write(const char *path);

/* Bytes transferred; 0 at end of file on read; negative on failure. */
int zio_read(zio_file_t *f, void *buf, int n);
int zio_write(zio_file_t *f, const void *buf, int n);

/* 0 on success. Closing a write handle is where a short write shows. */
int zio_close(zio_file_t *f);

/* Output. On the device this reaches the posix terminal if one is
 * running and the UART otherwise. */
void zio_out_open(void);
void zio_out(const char *s);
void zio_out_close(void);

/* A line straight to the machine's own console (the UART on the
 * device), whatever the output relay is doing: fatal errors are written
 * here too, so that a failure is never invisible. */
void zio_console(const char *s);

/* 1 where file names are 8.3 only: the device, whose FatFs is built
 * without long names (FF_USE_LFN 0). 0 on the host. */
int zio_fat83(void);

/* Milliseconds since some fixed point, for stage timings. */
uint32_t zio_ms(void);

/* Flushes output first. Every exit goes through here. */
void zio_exit(int status) __attribute__((noreturn));

/* The command line as one string, for a platform with no argv. Returns
 * 1 if there was one. The host build passes real argv instead. */
int zio_get_args(char *buf, int cap);

/* Raw memory for the arena, and its return. */
void *zio_block(size_t n);
void zio_free(void *p);

#endif
