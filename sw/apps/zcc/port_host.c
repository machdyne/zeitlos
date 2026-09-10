/*
 * zcc -- the development-host port. stdio and stdlib, nothing else.
 *
 * See zcc_port.h for what this is a port OF, and port_dev.c for the
 * Zeitlos side.
 */

#include <stdio.h>
#include <stdlib.h>

#include "zcc_port.h"

char *zio_read_file(const char *path, int *len) {

    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }

    char *buf = malloc((size_t)n + 2);
    if (!buf) { fclose(f); return NULL; }

    if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);

    buf[n] = 0;
    buf[n + 1] = 0;
    if (len) *len = (int)n;
    return buf;
}

int zio_write_file(const char *path, const void *buf, int len) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int ok = (len == 0) || (fwrite(buf, 1, (size_t)len, f) == (size_t)len);
    fclose(f);
    return ok ? 0 : -1;
}

/* stderr, not stdout: a compiler's diagnostics have to survive being
 * piped, and `zcc -E file.c > out.i` must not have errors land in the
 * preprocessed output. */
void zio_out(const char *s) {
    fputs(s, stderr);
    fflush(stderr);
}

void zio_exit(int status) {
    exit(status);
}

/* The host writes to stderr, which needs no opening. */
void zio_out_open(void) {}
void zio_out_close(void) {}

/* The host has a real argv. */
int zio_get_args(char *buf, int cap) {
    (void)buf;
    (void)cap;
    return 0;
}
