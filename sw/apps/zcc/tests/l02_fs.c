/*
 * The filesystem half of libz, against the simulator's host-backed
 * root (sim/simos.c). Write, read back, seek, chunk, delete.
 *
 * Runs identically under both compilers because both go through the
 * same syscalls -- which is the point: the runtime is the same object
 * either way, so a difference here would be zcc's calling convention
 * and nothing else.
 */

#include "libz.h"

static void show(const char *tag, int v) {
    printf("%s %d\n", tag, v);
}

int main(void) {
    static const char payload[] =
        "Zeitlos is a work-in-progress SOC and OS developed in tandem.\n";
    char buf[128];
    int n, h;

    n = fs_write_file("/zcctest.txt", (char *)payload, (int)sizeof(payload) - 1);
    show("wrote", n);

    show("size", fs_size("/zcctest.txt"));

    {
        char *whole = fs_mallocfile("/zcctest.txt");
        show("mallocfile", whole != 0);
        if (whole) { printf("text %s", whole); free(whole); }
    }

    /* chunked, with a seek back into the middle */
    h = fs_open_read("/zcctest.txt");
    show("open", h >= 0);
    if (h >= 0) {
        n = fs_read_chunk(h, buf, 8);
        buf[n] = 0;
        printf("chunk1 [%s]\n", buf);

        fs_seek(h, 11);
        n = fs_read_chunk(h, buf, 11);
        buf[n] = 0;
        printf("chunk2 [%s]\n", buf);

        fs_close_handle(h);
    }

    show("unlink", fs_unlink("/zcctest.txt"));
    show("gone", fs_size("/zcctest.txt"));

    /* a missing file is size 0 and a failed read, not a crash */
    show("missing_read", fs_mallocfile("/nope.txt") != 0);
    show("missing_open", fs_open_read("/nope.txt"));

    return 0;
}
