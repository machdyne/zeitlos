/* Extracted from sw/apps/read/read.c at test time -- this exercises
   the REAL rd_raw(), not a copy that can drift from it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#define RBUF 2048
static uint8_t rbuf[RBUF];
static uint32_t rbuf_base, rbuf_len, rbuf_pos;
static FILE *fp;
static int fh = 0;

static int fs_read_chunk(int h, void *b, uint32_t max) {
    (void)h; return (int)fread(b, 1, max, fp);
}

static bool rd_raw(char *out, int cap) {

	int n = 0;
	bool any = false;

	for (;;) {

		if (rbuf_pos >= rbuf_len) {

			// Exactly rd_byte()'s refill, hoisted out of the loop
			// over characters. rbuf_base must advance by the length
			// CONSUMED, before rbuf_len is replaced.
			rbuf_base += rbuf_len;
			rbuf_pos = 0;

			int r = fs_read_chunk(fh, rbuf, RBUF);
			rbuf_len = (r > 0) ? (uint32_t)r : 0;

			if (!rbuf_len) break;    // end of file
		}

		uint32_t i = rbuf_pos;
		while (i < rbuf_len && rbuf[i] != '\n') i++;

		for (uint32_t k = rbuf_pos; k < i; k++) {
			char ch = (char)rbuf[k];
			if (ch != '\r' && n < cap - 1) out[n++] = ch;
		}

		any = true;

		if (i < rbuf_len) {
			rbuf_pos = i + 1;        // consume the newline
			break;
		}

		// Ran out of buffered bytes before finding one: refill and
		// keep going on the same line.
		rbuf_pos = i;
	}

	out[n] = 0;
	return any || n > 0;

}


/* Reference: what the ORIGINAL per-byte reader produced. */
static int ref_byte(void) {
    int c = fgetc(fp);
    return (c == EOF) ? -1 : c;
}
static bool ref_raw(char *out, int cap) {
    int n = 0, c = ref_byte();
    if (c < 0) return false;
    while (c >= 0 && c != '\n') {
        if (c != '\r' && n < cap - 1) out[n++] = (char)c;
        c = ref_byte();
    }
    out[n] = 0;
    return true;
}

int main(int argc, char **argv) {
    int fails = 0, lines = 0;
    int caps[] = {1024, 64, 8};
    for (int a = 1; a < argc; a++) {
      for (int ci = 0; ci < 3; ci++) {
        int cap = caps[ci];
        static char g[8192], r[8192];

        fp = fopen(argv[a], "rb"); if (!fp) { perror(argv[a]); return 2; }
        rbuf_base = rbuf_len = rbuf_pos = 0;
        int ng = 0; static char *got[40000];
        bool ok;
        while ((ok = rd_raw(g, cap)) && ng < 40000) { got[ng++] = strdup(g); }
        uint32_t end_off = rbuf_base + rbuf_pos;
        fclose(fp);

        fp = fopen(argv[a], "rb");
        int nr = 0;
        while (ref_raw(r, cap)) {
            if (nr < ng && strcmp(got[nr], r)) {
                if (fails < 5)
                    printf("  MISMATCH %s cap=%d line %d:\n    new: '%s'\n    ref: '%s'\n",
                           argv[a], cap, nr, got[nr], r);
                fails++;
            }
            nr++;
        }
        long ref_end = ftell(fp);
        fclose(fp);

        if (nr != ng) {
            printf("  LINE COUNT %s cap=%d: new %d, ref %d\n", argv[a], cap, ng, nr);
            fails++;
        }
        if ((long)end_off != ref_end) {
            printf("  END OFFSET %s cap=%d: new %u, ref %ld\n",
                   argv[a], cap, end_off, ref_end);
            fails++;
        }
        for (int q=0;q<ng;q++) free(got[q]);
        lines += ng;
      }
    }
    printf("rd_raw: %d lines across %d file(s) x 3 caps, %d mismatch(es)\n",
           lines, argc-1, fails);
    return fails != 0;
}
