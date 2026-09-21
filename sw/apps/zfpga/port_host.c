/*
 * zfpga -- the host side of the seam. See zfpga_port.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "zfpga_port.h"

struct zio_file {
    FILE *fp;
};

static zio_file_t *wrap(FILE *fp) {
    zio_file_t *f;
    if (!fp) return NULL;
    f = malloc(sizeof(*f));
    if (!f) { fclose(fp); return NULL; }
    f->fp = fp;
    return f;
}

zio_file_t *zio_open_read(const char *path) {
    return wrap(fopen(path, "rb"));
}

zio_file_t *zio_open_write(const char *path) {
    return wrap(fopen(path, "wb"));
}

int zio_read(zio_file_t *f, void *buf, int n) {
    size_t r = fread(buf, 1, (size_t)n, f->fp);
    if (r == 0 && ferror(f->fp)) return -1;
    return (int)r;
}

int zio_write(zio_file_t *f, const void *buf, int n) {
    size_t w = fwrite(buf, 1, (size_t)n, f->fp);
    return (w == (size_t)n) ? n : -1;
}

int zio_close(zio_file_t *f) {
    int r = fclose(f->fp);
    free(f);
    return r == 0 ? 0 : -1;
}

void zio_out_open(void) {}

void zio_out(const char *s) {
    fputs(s, stdout);
}

void zio_out_close(void) {
    fflush(stdout);
}

void zio_exit(int status) {
    zio_out_close();
    exit(status);
}

int zio_get_args(char *buf, int cap) {
    (void)buf; (void)cap;
    return 0;
}

void *zio_block(size_t n) {
    return malloc(n);
}

void zio_free(void *p) {
    free(p);
}

void zio_console(const char *s) {
    (void)s;            /* the host's stdout is the console already */
}

uint32_t zio_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000u + t.tv_nsec / 1000000u);
}

int zio_fat83(void) {
    return 0;
}

int zio_remove(const char *path) {
    return remove(path);
}
