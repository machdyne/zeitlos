/* zeitlos32g's instruction-set simulator (t2g_iss.c) */
#include <stdint.h>
#include "../zgl_t2.h"
typedef struct {
	const uint32_t *in;           /* the stream */
	uint32_t in_n, in_i;
	uint32_t list[T2_RECS * T2_REC_WORDS];   /* the last GO's records */
	uint32_t nlist, rx, ry, clear, frames;
	uint64_t insns, cycles;
	int bad;
} t2g_run_t;
int t2g_load(const char *path);
void t2g_reset(void);
void t2g_run(t2g_run_t *r);
