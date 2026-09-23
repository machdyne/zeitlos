/*
 * rtl/tb/cache_soc/prog.c -- workload for rtl/tb/tb_cache_soc.v
 *
 * Runs on picorv32 or zeitlos32 against the real cache and SDRAM
 * controller. Prints through a UART at 0xf0000000 and reports a
 * checksum through DONE (0xf0000004), which the testbench compares
 * against the same program's result on a build with no cache at all.
 *
 * Phases are bracketed with MARK writes (0xf0000008) so the testbench
 * can report cycles per phase.
 */
#include <stdint.h>

#define UART  (*(volatile uint32_t *)0xf0000000)
#define MARK  (*(volatile uint32_t *)0xf0000008)
#define FAIL  (*(volatile uint32_t *)0xf000000c)

#define I_CTRL (*(volatile uint32_t *)0x70000100)
#define D_CTRL (*(volatile uint32_t *)0x70000110)
#define D_INFO (*(volatile uint32_t *)0x7000011c)
#define I_SNOOPS (*(volatile uint32_t *)0x70000134)

/* tb_cache_soc.v -DARB: a second master writes an increasing count to
 * MAILBOX. WRITER reads 1 when it is present. */
#define WRITER  (*(volatile uint32_t *)0xf0000010)
#define MAILBOX (*(volatile uint32_t *)0x4000f000)

#define MPU_CTRL  (*(volatile uint32_t *)0x90000100)
#define MPU_KTEXT (*(volatile uint32_t *)0x90000104)
#define MPU_GATE  (*(volatile uint32_t *)0x90000108)
#define MPU_SIZE  (*(volatile uint32_t *)0x9000010c)
#define MPU_MASK  (*(volatile uint32_t *)0x90000110)
#define MPU_FADDR (*(volatile uint32_t *)0x90000114)
#define MPU_FINFO (*(volatile uint32_t *)0x9000011c)
#define MPU_COUNT (*(volatile uint32_t *)0x90000120)
#define MPU_INFO  (*(volatile uint32_t *)0x90000124)
#define APPBASE   0x40008000u      /* tb_cache_soc.v's MTU base */
#define DONE      (*(volatile uint32_t *)0xf0000004)
extern char app_code[], app_code_end[], _bss_end[];

static void putc_(char c) { UART = (uint8_t)c; }
static void puts_(const char *s) { while (*s) putc_(*s++); }
static void puthex(uint32_t v) {
	for (int i = 28; i >= 0; i -= 4) putc_("0123456789abcdef"[(v >> i) & 15]);
}

/* -- code loading: the case the I-side snoop exists for -------------- */

typedef uint32_t (*fn_t)(uint32_t);

__attribute__((section(".text.exec_a"), noinline))
uint32_t exec_a(uint32_t x) { return x * 3 + 0x1111; }

__attribute__((section(".text.exec_b"), noinline))
uint32_t exec_b(uint32_t x) { return (x ^ 0x5a5a) + 0x2222; }

static uint32_t exec_buf[16] __attribute__((aligned(64)));

static void load_code(const void *src) {
	const volatile uint32_t *s = (const volatile uint32_t *)src;
	volatile uint32_t *d = exec_buf;
	for (int i = 0; i < 16; i++) d[i] = s[i];
}

/* -- data workloads ------------------------------------------------- */

#define N 512
static uint32_t arr[N];
static uint32_t copy[N];
static char text[1024];

struct node { struct node *next; uint32_t val; uint32_t pad[2]; };
static struct node nodes[256];

static uint32_t rng = 12345;
static uint32_t rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }

static void isort(uint32_t *a, int n) {
	for (int i = 1; i < n; i++) {
		uint32_t v = a[i]; int j = i - 1;
		while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; j--; }
		a[j + 1] = v;
	}
}

static uint32_t fib(uint32_t n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

static void mcopy(void *d, const void *s, int n) {
	uint8_t *dd = d; const uint8_t *ss = s;
	while (n--) *dd++ = *ss++;
}

static uint32_t hash(const char *s) {
	uint32_t h = 2166136261u;
	while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
	return h;
}

static uint32_t reg_loop(uint32_t n) {
	uint32_t a = 1, b = 2, c = 3;
	for (uint32_t i = 0; i < n; i++) { a += b; b ^= c; c += a >> 3; }
	return a + b + c;
}

static volatile uint32_t mpu_kernword;
static uint32_t g_sum;

/* The gate: where the app goes when it is done. Privileged again
 * (fetched from kernel code at the GATE address). Checks what the app
 * managed to do and finishes the test; never returns. */
__attribute__((noreturn, noinline)) void mpu_kernel_continue(void) {
	uint32_t own = *(volatile uint32_t *)(APPBASE + 0x800);
	uint32_t info = MPU_FINFO, count = MPU_COUNT, faddr = MPU_FADDR;
	int ok = 1;
	if (own != 0x1111) { puts_("MPU: app's own store lost\n"); ok = 0; }
	if (mpu_kernword != 0) { puts_("MPU: app wrote kernel memory\n"); ok = 0; }
	if (!(info >> 31) || ((info >> 24) & 15) != 1 || ((info >> 16) & 3) != 2 ||
	    faddr != (uint32_t)&mpu_kernword || count != 1) {
		puts_("MPU: fault registers wrong "); puthex(info); putc_(' ');
		puthex(faddr); putc_(' '); puthex(count); putc_('\n'); ok = 0;
	}
	if (ok) puts_("mpu: app store blocked and reported, own store kept\n");
	else FAIL = 7;
	DONE = g_sum;
	for (;;) ;
}

int main(void) {
	uint32_t sum = 0, r;
	int dcache = ((D_INFO >> 16) == 0x1DCA);
	int mpu = ((MPU_INFO >> 16) == 0x3A50);

	/* tb_cache_soc.v -DMPU: run everything below under an enforcing
	 * MPU. This program is the "kernel": its code, data and bss count
	 * as kernel code (exec_buf in bss is executed), so KTEXT covers up
	 * to the end of bss. The app block is APPBASE, 4KB. */
	if (mpu) {
		MPU_KTEXT = (uint32_t)_bss_end;
		MPU_GATE = (uint32_t)mpu_kernel_continue;
		MPU_SIZE = 0x1000;
		MPU_MASK = 0xF7FF;
		MPU_CTRL = 0x3;              /* enable + enforce, no irq */
	}

	puts_("cache_soc: ");
	if (dcache) {
		D_CTRL = 0x5;              /* enable D + posted writes */
		puts_("dcache on\n");
	} else {
		puts_("no dcache\n");
	}

	/* 1. code loading: A, run, then B over it, run. The unified
	 *    cache must get this right with NO flush; wb_icache needs the
	 *    flush that fs_load_exec() does, so only issue it there. */
	MARK = 1;
	load_code((const void *)exec_a);
	if (!dcache) I_CTRL = 3;
	r = ((fn_t)exec_buf)(7);
	if (r != 7 * 3 + 0x1111) { puts_("EXEC A FAIL "); puthex(r); putc_('\n'); FAIL = 1; }
	sum += r;
	for (int k = 0; k < 3; k++) r = ((fn_t)exec_buf)(k);   /* keep it hot */
	load_code((const void *)exec_b);
	if (!dcache) I_CTRL = 3;
	r = ((fn_t)exec_buf)(7);
	if (r != ((7 ^ 0x5a5a) + 0x2222)) { puts_("EXEC B FAIL "); puthex(r); putc_('\n'); FAIL = 2; }
	sum += r;
	/* and back again: byte stores this time */
	{
		const volatile uint8_t *s = (const volatile uint8_t *)exec_a;
		volatile uint8_t *d = (volatile uint8_t *)exec_buf;
		for (int i = 0; i < 64; i++) d[i] = s[i];
	}
	if (!dcache) I_CTRL = 3;
	r = ((fn_t)exec_buf)(9);
	if (r != 9 * 3 + 0x1111) { puts_("EXEC A2 FAIL "); puthex(r); putc_('\n'); FAIL = 3; }
	sum += r;
	if (dcache) { puts_("i-snoops "); puthex(I_SNOOPS); putc_('\n'); }

	/* 2. register-only loop (fetch bound) */
	MARK = 2;
	sum += reg_loop(1500);

	/* 3. sort (load/store heavy, good locality) */
	MARK = 3;
	for (int i = 0; i < N; i++) arr[i] = rnd();
	isort(arr, 120);
	for (int i = 1; i < 120; i++) if (arr[i - 1] > arr[i]) { FAIL = 4; }
	sum += arr[0] + arr[119];

	/* 4. byte memcpy (store heavy) */
	MARK = 4;
	for (int k = 0; k < 2; k++) mcopy(copy, arr, sizeof(arr));
	for (int i = 0; i < N; i++) sum += copy[i];

	/* 5. linked list walk (pointer chasing) */
	MARK = 5;
	for (int i = 0; i < 256; i++) { nodes[i].val = i * 7; nodes[i].next = &nodes[(i * 97 + 13) & 255]; }
	{
		struct node *p = &nodes[0];
		for (int i = 0; i < 1500; i++) { sum += p->val; p = p->next; }
	}

	/* 6. strings + recursion (stack heavy) */
	MARK = 6;
	for (int i = 0; i < 1023; i++) text[i] = 'a' + (rnd() % 26);
	text[1023] = 0;
	for (int k = 0; k < 2; k++) sum += hash(text);
	sum += fib(12);

	MARK = 7;
	puts_("sum "); puthex(sum); putc_('\n');

	/* 8. the MPU test app: copy it into its block and jump in */
	if (mpu) {
		volatile uint32_t *d = (volatile uint32_t *)APPBASE;
		const volatile uint32_t *src = (const volatile uint32_t *)app_code;
		for (int i = 0; i < (app_code_end - app_code) / 4; i++) d[i] = src[i];
		if (!dcache) I_CTRL = 3;
		g_sum = sum;
		register uint32_t a0 __asm__("a0") = APPBASE + 0x800;
		register uint32_t a1 __asm__("a1") = (uint32_t)&mpu_kernword;
		register uint32_t a2 __asm__("a2") = (uint32_t)mpu_kernel_continue;
		register uint32_t a3 __asm__("a3") = 0x1111;
		__asm__ volatile ("jr %4" :: "r"(a0), "r"(a1), "r"(a2), "r"(a3), "r"(APPBASE) : "memory");
		for (;;) ;
	}

	/* 7. another master writing memory the CPU has cached. MAILBOX is
	 *    read through the data cache; it can only be seen to change if
	 *    the other master's writes invalidate the line (the snoop).
	 *    Not part of the checksum: the values are timing dependent. */
	if (WRITER) {
		uint32_t last = MAILBOX, v;
		for (int k = 0; k < 6; k++) {
			int spins = 0;
			while ((v = MAILBOX) == last) {
				if (++spins > 100000) { puts_("MAILBOX STUCK\n"); FAIL = 5; return (int)sum; }
			}
			if (v < last) { puts_("MAILBOX WENT BACKWARDS\n"); FAIL = 6; }
			last = v;
		}
		puts_("mailbox ok "); puthex(last); putc_('\n');
	}
	return (int)sum;
}
