/*
 * Host test for the kernel's memory pool, sw/os/mem.c:
 *
 *   cc -std=gnu99 -Wall -no-pie -I sw/os -I sw/common -o /tmp/test_mem \
 *      sw/os/tests/test_mem.c && /tmp/test_mem
 *
 * mem.c is included rather than linked so the test can walk its
 * block list. Addresses are the pool's real ones (0x4000_0000 up);
 * nothing here dereferences them.
 *
 *   churn    alloc + free, over and over. Every allocation that splits
 *            a free block takes a descriptor, and the pool used to
 *            take them without ever giving one back: the 255th
 *            allocation since boot failed for good, however much was
 *            free. Each app launch is one allocation.
 *   random   allocations and frees in random order, checked after
 *            every step against what the test thinks it holds: no
 *            overlap, every block inside the pool, every block 4KB
 *            aligned, descriptors in use == blocks in the list, and
 *            no two free blocks side by side (coalescing worked).
 *   full     exhaust the pool, get NULL, free, and allocate again.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../mem.c"

// mem.c's Z_SYS_MEM_STATS handler returns these; nothing else is used
z_obj_t z_ok, z_fail;

static int failed, run;

#define CHECK(c, ...) do { run++; if (!(c)) { failed++; \
	printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
	printf("\n"); } } while (0)

#define POOL (8u << 20)

static int list_len(void) {
	int n = 0;
	for (k_mem_block_t *b = (k_mem_block_t *)block_list; b; b = b->next) n++;
	return n;
}

// The block list's invariants, whatever the test has allocated.
static void check_list(const char *when) {
	uint32_t at = Z_MEM_BASE, total = 0;
	bool prev_free = false;
	int n = 0;
	for (k_mem_block_t *b = (k_mem_block_t *)block_list; b; b = b->next) {
		n++;
		CHECK(b->start == at, "%s: block %d starts at %08x, expected %08x",
			when, n, b->start, at);
		CHECK(b->size > 0, "%s: block %d is empty", when, n);
		if (b->used)
			CHECK((b->start & (Z_MEM_ALIGNMENT - 1)) == 0,
				"%s: used block at %08x not aligned", when, b->start);
		CHECK(!(prev_free && !b->used),
			"%s: two free blocks side by side at %08x", when, b->start);
		prev_free = !b->used;
		at = b->start + b->size;
		total += b->size;
		if (n > Z_MEM_MAX_BLOCKS + 1) break;
	}
	CHECK(total == POOL, "%s: blocks cover %u of %u bytes", when, total, POOL);
	CHECK(n == mem_desc_live, "%s: %d blocks in the list, %d descriptors live",
		when, n, mem_desc_live);
}

static void test_churn(void) {
	k_mem_init(POOL);
	int i;
	for (i = 0; i < 5000; i++) {
		void *p = k_mem_alloc(64 * 1024);
		if (!p) break;
		k_mem_free(p);
	}
	CHECK(i == 5000, "churn: allocation %d failed", i);
	CHECK(list_len() == 1, "churn: %d blocks left, expected one free block",
		list_len());
	check_list("churn");

	// two live blocks with the churn between them, like the boot set
	// staying up while apps come and go
	void *a = k_mem_alloc(100 * 1024);
	for (i = 0; i < 5000; i++) {
		void *p = k_mem_alloc(40 * 1024 + (i % 7) * 4096);
		void *q = k_mem_alloc(64 * 1024);
		if (!p || !q) break;
		k_mem_free(p);
		k_mem_free(q);
	}
	CHECK(i == 5000, "churn with a live block: iteration %d failed", i);
	k_mem_free(a);
	CHECK(list_len() == 1, "churn: %d blocks after freeing all", list_len());
	check_list("churn 2");
}

#define SLOTS 48

static void test_random(void) {
	void *held[SLOTS];
	uint32_t sizes[SLOTS];
	memset(held, 0, sizeof held);
	k_mem_init(POOL);
	srand(7);
	for (int step = 0; step < 200000; step++) {
		int s = rand() % SLOTS;
		if (held[s]) {
			k_mem_free(held[s]);
			held[s] = NULL;
		} else {
			uint32_t sz = (uint32_t)(rand() % (256 * 1024)) + 1;
			held[s] = k_mem_alloc(sz);
			sizes[s] = k_mem_align_up(sz < Z_MEM_MIN_BLOCK_SIZE ?
				Z_MEM_MIN_BLOCK_SIZE : sz, Z_MEM_ALIGNMENT);
			// 48 x 256KB is 12MB against an 8MB pool: some fail, which
			// is the point -- a failure must leave the pool intact
		}
		if ((step & 1023) == 0 || step < 2000) {
			check_list("random");
			// what the test holds must not overlap
			for (int x = 0; x < SLOTS; x++) for (int y = x + 1; y < SLOTS; y++) {
				if (!held[x] || !held[y]) continue;
				uint32_t ax = (uint32_t)(uintptr_t)held[x], ay = (uint32_t)(uintptr_t)held[y];
				CHECK(ax + sizes[x] <= ay || ay + sizes[y] <= ax,
					"random: %08x+%u overlaps %08x+%u", ax, sizes[x], ay, sizes[y]);
			}
			if (failed > 20) return;
		}
	}
	for (int s = 0; s < SLOTS; s++) if (held[s]) k_mem_free(held[s]);
	CHECK(list_len() == 1, "random: %d blocks after freeing all", list_len());
	check_list("random end");
	CHECK(mem_block_count <= 2 * SLOTS + 4,
		"random: %d descriptors ever taken for %d live blocks at most",
		mem_block_count, SLOTS);
}

static void test_full(void) {
	void *p[300];
	int n = 0;
	k_mem_init(POOL);
	while (n < 300 && (p[n] = k_mem_alloc(64 * 1024))) n++;
	CHECK(n == (int)(POOL / (64 * 1024)), "full: %d blocks of 64KB, expected %u",
		n, POOL / (64 * 1024));
	CHECK(k_mem_alloc(4096) == NULL, "full: allocation succeeded on a full pool");
	for (int i = 0; i < n; i += 2) k_mem_free(p[i]);
	CHECK(k_mem_alloc(128 * 1024) == NULL,
		"full: 128KB from a pool of 64KB holes");
	void *q = k_mem_alloc(64 * 1024);
	CHECK(q != NULL, "full: 64KB into a 64KB hole failed");
	k_mem_free(q);
	for (int i = 1; i < n; i += 2) k_mem_free(p[i]);
	CHECK(list_len() == 1, "full: %d blocks after freeing all", list_len());
	check_list("full");
	CHECK(k_mem_alloc(POOL) != NULL, "full: the whole pool in one block failed");
}

// Arenas: from the top, resized in place up and down, grown down into
// the hole below, and the list kept whole throughout.
static void test_arena(void) {
	k_mem_init(POOL);
	void *app1 = k_mem_alloc(256 * 1024);
	void *a = k_mem_alloc_top(8 * 1024);
	CHECK(a && (uint32_t)(uintptr_t)a == Z_MEM_BASE + POOL - 8 * 1024,
		"arena: first one at the very top (%p)", a);
	CHECK(k_mem_block_size(a) == 8 * 1024, "arena: 8KB, no 32KB minimum");
	void *b = k_mem_alloc_top(5000);
	CHECK(b && (uint32_t)(uintptr_t)b == (uint32_t)(uintptr_t)a - 8192,
		"arena: the next one right below it, rounded to 8KB");
	check_list("arena alloc");

	// a is at the top: nothing above it, so it cannot grow in place
	CHECK(!k_mem_resize(a, 64 * 1024), "arena: no room above the top one");
	// b can shrink, and grow back into what it gave up
	CHECK(k_mem_resize(b, 4096) && k_mem_block_size(b) == 4096, "arena: shrink in place");
	check_list("arena shrink");
	CHECK(k_mem_resize(b, 8192) && k_mem_block_size(b) == 8192, "arena: grow back in place");
	check_list("arena regrow");

	// b grows down into the free space below it, keeping its end
	uint32_t bend = (uint32_t)(uintptr_t)b + 8192;
	uint32_t nb = k_mem_grow_down(b, 1024 * 1024);
	CHECK(nb && nb + 1024 * 1024 == bend, "arena: grown down, same end");
	CHECK(k_mem_block_size((void *)(uintptr_t)nb) == 1024 * 1024, "arena: 1MB now");
	check_list("arena grow down");

	// process images still come from the bottom
	void *app2 = k_mem_alloc(64 * 1024);
	CHECK(app2 && (uint32_t)(uintptr_t)app2 < nb && (uint32_t)(uintptr_t)app2 > (uint32_t)(uintptr_t)app1,
		"arena: an app image goes low, above the first");

	// free everything: one free block, no descriptor lost
	k_mem_free((void *)(uintptr_t)nb);
	k_mem_free(a);
	k_mem_free(app1);
	k_mem_free(app2);
	CHECK(list_len() == 1 && k_mem_free_bytes() == POOL, "arena: all back");
	check_list("arena end");

	// churn: resize up and down a thousand times
	void *c = k_mem_alloc_top(4096);
	int i;
	for (i = 0; i < 2000; i++) {
		uint32_t sz = 4096u * (uint32_t)(1 + (i * 7) % 300);
		if (!k_mem_resize(c, sz)) {
			uint32_t d = k_mem_grow_down(c, sz);
			if (!d) break;
			c = (void *)(uintptr_t)d;
		}
	}
	CHECK(i == 2000, "arena churn: step %d failed", i);
	check_list("arena churn");
	k_mem_free(c);
	CHECK(list_len() == 1, "arena churn: %d blocks after freeing", list_len());
}

int main(void) {
	test_arena();
	test_churn();
	test_random();
	test_full();
	printf("%s: %d checks, %d failed\n", failed ? "FAIL" : "ok", run, failed);
	return failed ? 1 : 0;
}
