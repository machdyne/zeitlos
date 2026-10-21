/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * ipcbench -- Phase 0 of docs/userspace_drivers.md: what a driver
 * moved out of the kernel would cost, measured on the machine.
 *
 *   run ipcbench          (from the kernel shell; output on the console)
 *
 * One binary, several roles. The first copy registers "ipcb" and is
 * the master; it starts more copies of itself with z_proc_run(), and
 * each one, finding the master already registered, asks it for a role:
 *
 *   ECHO     replies to every PING -- the "server" of a round trip
 *   SPIN     burns CPU, to show what round-robin does to a round trip
 *   SLEEP    z_proc_wait(1) in a loop, recording the longest gap
 *            between wakeups -- how long the scheduler was held
 *
 * Every figure is rdcycle at Z_SYSCLK_HZ (48MHz). The test file is
 * written to the card root and deleted afterwards; with no card the
 * card tests are skipped.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zfsapp.h"
#include "../../common/zfsc.h"
#include "../../common/zcycles.h"
#include "../../common/zsoc.h"
#include "../../common/zsubjects.h"
#include "../../common/zproc.h"
#include "../../common/zpaths.h"

#define SUBJ_HELLO   (Z_SUBJ_BENCH + 0)
#define SUBJ_ROLE    (Z_SUBJ_BENCH + 1)
#define SUBJ_PING    (Z_SUBJ_BENCH + 2)
#define SUBJ_PONG    (Z_SUBJ_BENCH + 3)
#define SUBJ_STOP    (Z_SUBJ_BENCH + 4)
#define SUBJ_RESET   (Z_SUBJ_BENCH + 5)
#define SUBJ_REPORT  (Z_SUBJ_BENCH + 6)
#define SUBJ_STATS   (Z_SUBJ_BENCH + 7)
#define SUBJ_STYLE   (Z_SUBJ_BENCH + 8)   // echo: which receive to use
#define SUBJ_GBUF    (Z_SUBJ_BENCH + 9)   // echo: the master's buffer pointer
#define SUBJ_GFILL   (Z_SUBJ_BENCH + 10)   // echo: fill it through the grant
#define SUBJ_GREAD   (Z_SUBJ_BENCH + 11)   // echo: read a file into it

Z_SUBJECTS_IN(Z_SUBJ_BENCH, SUBJ_GREAD);

#define ROLE_ECHO    1
#define ROLE_SPIN    2
#define ROLE_SLEEP   3

#define N_SPIN       3
#define RT_ITERS     1000
#define CHUNK        8192
#define FILE_BYTES   (512u * 1024u)
#define SMALL_ITERS  200

#define CYC_PER_US   (Z_SYSCLK_HZ / 1000000u)

static char g_name[16];
static uint32_t g_master;

// 1: receive with z_msg_recv() (one syscall); 0: z_msg_read(),
// z_proc_wait(), z_msg_read() -- the loop every app used before it.
static int g_recv_style;
static int g_has_recv;

// -- messaging helpers -------------------------------------------------

// Blocking receive. z_msg_wait() polls, which would make every figure
// here a measurement of the poll loop; this sleeps between messages.
static void recv(z_msg_t *m) {
	if (g_recv_style) {
		while (z_msg_recv(m, 0) != Z_OK) ;
		return;
	}
	while (z_msg_read(m) != Z_OK) z_proc_wait(0);
}

// Does this kernel have Z_SYS_MSG_RECV? An empty mailbox and a 1-tick
// timeout: a kernel that has it sleeps at least a tick (65,536
// cycles); one that does not fails the unknown syscall at once.
static int probe_recv(void) {
	z_msg_t m;
	uint32_t t0 = z_cycles();
	if (z_msg_recv(&m, 1) == Z_OK) return 1;	// a message: it works
	return (z_cycles() - t0) > 30000u;
}

static void recv_subj(z_msg_t *m, uint32_t subj) {
	do recv(m); while (m->subject != subj);
}

static void send(uint32_t to, uint32_t subj, uint32_t v) {
	z_msg_new_send(to, subj, 0, z_obj_uint32(v));
}

// -- statistics --------------------------------------------------------

typedef struct { uint32_t n, min, max; uint64_t sum; } stat_t;

static void st_init(stat_t *s) { s->n = 0; s->min = 0xffffffffu; s->max = 0; s->sum = 0; }
static void st_add(stat_t *s, uint32_t v) {
	s->n++; s->sum += v;
	if (v < s->min) s->min = v;
	if (v > s->max) s->max = v;
}

// cycles -> "us" with one decimal, without floating point
static void pr_us(uint32_t cyc) {
	uint32_t t = (cyc * 10u + CYC_PER_US / 2) / CYC_PER_US;
	printf("%lu.%lu", (unsigned long)(t / 10), (unsigned long)(t % 10));
}

static void st_print(const char *what, stat_t *s) {
	uint32_t avg = s->n ? (uint32_t)(s->sum / s->n) : 0;
	printf("  %-30s n=%-5lu avg %7lu cyc (", what, (unsigned long)s->n, (unsigned long)avg);
	pr_us(avg); printf(" us)  min "); pr_us(s->min);
	printf("  max "); pr_us(s->max); printf(" us\n");
}

// -- the worker roles --------------------------------------------------

static void role_echo(void) {
	z_msg_t m;
	uint32_t client_ptr = 0;
	char *pat = malloc(CHUNK);
	if (pat) for (int i = 0; i < CHUNK; i++) pat[i] = (char)(i * 7 + 3);
	for (;;) {
		recv(&m);
		if (m.subject == SUBJ_PING) send(m.from, SUBJ_PONG, m.obj.val.uint32);
		else if (m.subject == SUBJ_STYLE) {
			g_recv_style = (int)m.obj.val.uint32 && g_has_recv;
			send(m.from, SUBJ_PONG, (uint32_t)g_recv_style);
		} else if (m.subject == SUBJ_GBUF) {
			client_ptr = m.obj.val.uint32;
			send(m.from, SUBJ_PONG, 0);
		} else if (m.subject == SUBJ_GFILL) {
			// Through the grant window, or not at all: a store into
			// the master's memory without it is an MPU fault, which
			// would end this process. The result says which.
			uint32_t base, size, ok = 0;
			if (pat && z_grant_open(m.from, &base, &size) == Z_OK) {
				memcpy(z_grant_ptr(base, (void *)client_ptr), pat, CHUNK);
				z_grant_close();
				ok = 1;
			}
			send(m.from, SUBJ_PONG, ok);
		} else if (m.subject == SUBJ_GREAD) {
			// What a filesystem server would do: read a file straight
			// into the client's buffer. The kernel accepts the grant
			// window as a syscall buffer, as the MPU accepts it for
			// stores.
			uint32_t base, size, total = 0xffffffffu;
			const char *path = (m.obj.type == Z_STR) ? m.obj.val.str : NULL;
			if (path && z_grant_open(m.from, &base, &size) == Z_OK) {
				void *dst = z_grant_ptr(base, (void *)client_ptr);
				int h = fs_open_read(path);
				if (h >= 0) {
					int n;
					total = 0;
					while ((n = fs_read_chunk(h, dst, CHUNK)) > 0) total += (uint32_t)n;
					if (n < 0) total = 0xfffffffeu;
					fs_close_handle(h);
				}
				z_grant_close();
			}
			send(m.from, SUBJ_PONG, total);
		}
		else if (m.subject == SUBJ_STOP) return;
	}
}

static void role_spin(void) {
	z_msg_t m;
	volatile uint32_t x = 0;
	for (;;) {
		for (int i = 0; i < 4096; i++) x += i;
		if (z_msg_read(&m) == Z_OK && m.subject == SUBJ_STOP) return;
	}
}

static void role_sleep(void) {
	z_msg_t m;
	uint32_t max = 0, n = 0;
	uint32_t t = z_cycles();
	for (;;) {
		z_proc_wait(1);
		uint32_t now = z_cycles();
		uint32_t dt = now - t;
		t = now;
		while (z_msg_read(&m) == Z_OK) {
			if (m.subject == SUBJ_RESET) { max = 0; n = 0; }
			else if (m.subject == SUBJ_REPORT) {
				// two numbers, two messages: keeps the payload a plain uint32
				send(m.from, SUBJ_STATS, max);
				send(m.from, SUBJ_STATS, n);
			} else if (m.subject == SUBJ_STOP) return;
			dt = 0;   // a message woke us early: not a scheduling gap
			t = z_cycles();
		}
		if (dt) { n++; if (dt > max) max = dt; }
	}
}

static int worker(void) {
	z_msg_t m;
	g_has_recv = probe_recv();
	send(g_master, SUBJ_HELLO, z_getpid());
	recv_subj(&m, SUBJ_ROLE);
	switch (m.obj.val.uint32) {
	case ROLE_ECHO:  role_echo();  break;
	case ROLE_SPIN:  role_spin();  break;
	case ROLE_SLEEP: role_sleep(); break;
	}
	return 0;
}

// -- the master --------------------------------------------------------

// Where to start workers from: a process does not know its own path,
// so the usual places, and the first that works is kept -- ipcbench
// fetched with `tget` to /ram runs as well as one in /apps.
static const char *const self_paths[] = {
	"ipcbench", Z_DIR_RAM "/ipcbench", Z_DIR_TMP "/ipcbench", Z_DIR_USB "/ipcbench",
};
static int self_at = -1;

static uint32_t spawn(uint32_t role) {
	z_msg_t m;
	bool ok = false;
	if (self_at >= 0) ok = z_proc_run(self_paths[self_at]);
	for (int i = 0; !ok && i < (int)(sizeof(self_paths) / sizeof(self_paths[0])); i++)
		if (z_proc_run(self_paths[i])) { self_at = i; ok = true; }
	if (!ok) {
		printf("ipcbench: could not start a worker (ipcbench not in " Z_DIR_APPS
			", " Z_DIR_RAM ", " Z_DIR_TMP " or " Z_DIR_USB ")\n");
		return 0;
	}
	recv_subj(&m, SUBJ_HELLO);
	uint32_t pid = m.obj.val.uint32;
	send(pid, SUBJ_ROLE, role);
	return pid;
}

static void round_trips(uint32_t echo, const char *label) {
	z_msg_t m;
	stat_t s;
	st_init(&s);
	for (uint32_t i = 0; i < RT_ITERS; i++) {
		uint32_t t0 = z_cycles();
		send(echo, SUBJ_PING, i);
		recv_subj(&m, SUBJ_PONG);
		st_add(&s, z_cycles() - t0);
	}
	st_print(label, &s);
}

// The scheduler trace (docs/kernel.md, "Scheduler trace"): only a
// kernel built with KTRACE=1 has it; any other kernel fails the call.
static bool ktrace(uint32_t on) {
	z_obj_t obj = z_obj_uint32(on);
	z_kernel_ptr_t k = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	z_obj_t *rv = (z_obj_t *)k(Z_SYS_KTRACE, (uint32_t *)&obj, 0);
	return rv && rv->type == Z_RETVAL && rv->val.uint32 == 0;
}

static uint32_t sleeper_max(uint32_t sl, uint32_t *n) {
	z_msg_t m;
	send(sl, SUBJ_REPORT, 0);
	recv_subj(&m, SUBJ_STATS); uint32_t max = m.obj.val.uint32;
	recv_subj(&m, SUBJ_STATS); *n = m.obj.val.uint32;
	return max;
}

static void stall_report(uint32_t sl, const char *label) {
	uint32_t n, max = sleeper_max(sl, &n);
	printf("  %-30s longest gap ", label);
	pr_us(max);
	printf(" us over %lu wakeups (a tick is ", (unsigned long)n);
	pr_us(65536u);
	printf(" us)\n");
}

static void sleeper_reset(uint32_t sl) {
	send(sl, SUBJ_RESET, 0);
	z_proc_wait(4);   // let it take the reset and settle
}

static void bench_cpu(void) {
	stat_t s;
	printf("\n[1] baseline costs\n");

	st_init(&s);
	for (int i = 0; i < 2000; i++) {
		uint32_t t0 = z_cycles();
		(void)z_getpid();
		st_add(&s, z_cycles() - t0);
	}
	st_print("syscall (z_getpid)", &s);

	st_init(&s);
	for (int i = 0; i < 2000; i++) {
		z_msg_t m;
		uint32_t t0 = z_cycles();
		(void)z_msg_read(&m);
		st_add(&s, z_cycles() - t0);
	}
	st_print("z_msg_read, empty mailbox", &s);

	char *a = malloc(CHUNK), *b = malloc(CHUNK);
	if (a && b) {
		memset(a, 0x5a, CHUNK);
		st_init(&s);
		for (int i = 0; i < 100; i++) {
			uint32_t t0 = z_cycles();
			memcpy(b, a, CHUNK);
			__asm__ volatile ("" ::: "memory");
			st_add(&s, z_cycles() - t0);
		}
		st_print("memcpy 8KB", &s);
	}
	free(a); free(b);
}

static void bench_fs_small(const char *path, const char *label) {
	stat_t s;
	z_fs_info_t info;
	char buf[64];

	// make sure it exists
	if (fs_write_file((char *)path, (char *)"x", 1) < 0) {
		printf("  %s: cannot write %s, skipped\n", label, path);
		return;
	}
	snprintf(buf, sizeof buf, "%s stat", label);
	st_init(&s);
	for (int i = 0; i < SMALL_ITERS; i++) {
		uint32_t t0 = z_cycles();
		fs_stat(path, &info);
		st_add(&s, z_cycles() - t0);
	}
	st_print(buf, &s);

	snprintf(buf, sizeof buf, "%s open+close", label);
	st_init(&s);
	for (int i = 0; i < SMALL_ITERS; i++) {
		uint32_t t0 = z_cycles();
		int h = fs_open_read(path);
		if (h >= 0) fs_close_handle(h);
		st_add(&s, z_cycles() - t0);
	}
	st_print(buf, &s);
	fs_unlink((char *)path);
}

static void pr_kbs(const char *label, uint32_t bytes, uint32_t cyc) {
	// KB/s = bytes / (cyc / clk) / 1024
	uint64_t kbs = (uint64_t)bytes * Z_SYSCLK_HZ / cyc / 1024u;
	printf("  %-30s %lu KB/s (%lu bytes, ", label, (unsigned long)kbs, (unsigned long)bytes);
	pr_us(cyc);
	printf(" us)\n");
}

// Write the file, then read it back while the sleeper watches.
static int g_grant;	// the grant window works (set by bench_grant())

static void bench_fs_bulk(const char *path, const char *label, uint32_t sl, uint32_t echo) {
	char *buf = malloc(CHUNK);
	char l[64];
	if (!buf) return;
	memset(buf, 0xa5, CHUNK);

	int h = fs_open_write(path);
	if (h < 0) { printf("  %s: cannot create %s, skipped\n", label, path); free(buf); return; }
	sleeper_reset(sl);
	uint32_t t0 = z_cycles(), done = 0;
	while (done < FILE_BYTES) {
		if (fs_write_chunk(h, buf, CHUNK) != CHUNK) break;
		done += CHUNK;
	}
	fs_close_handle(h);
	uint32_t cyc = z_cycles() - t0;
	snprintf(l, sizeof l, "%s write 8KB chunks", label);
	pr_kbs(l, done, cyc);
	snprintf(l, sizeof l, "  ...stall during write");
	stall_report(sl, l);

	h = fs_open_read(path);
	if (h >= 0) {
		sleeper_reset(sl);
		stat_t s; st_init(&s);
		t0 = z_cycles(); done = 0;
		for (;;) {
			uint32_t c0 = z_cycles();
			int n = fs_read_chunk(h, buf, CHUNK);
			if (n <= 0) break;
			st_add(&s, z_cycles() - c0);
			done += n;
		}
		cyc = z_cycles() - t0;
		fs_close_handle(h);
		snprintf(l, sizeof l, "%s read 8KB chunks", label);
		pr_kbs(l, done, cyc);
		snprintf(l, sizeof l, "%s one 8KB read_chunk", label);
		st_print(l, &s);
		stall_report(sl, "  ...stall during chunked read");
	}

	// (The "read, by another process" test is gone: it modelled what
	// fs now does itself, and a driver's grant-window pointer is a
	// physical address that cannot travel in a message to fs.)
	free(buf);

	// a whole-file read: one syscall, the scheduler held throughout
	char *big = malloc(FILE_BYTES);
	if (big) {
		sleeper_reset(sl);
		t0 = z_cycles();
		int n = fs_read_file((char *)path, big, FILE_BYTES);
		cyc = z_cycles() - t0;
		snprintf(l, sizeof l, "%s whole-file read", label);
		if (n > 0) pr_kbs(l, (uint32_t)n, cyc);
		stall_report(sl, "  ...stall during whole-file read");
		free(big);
	}
	fs_unlink((char *)path);
}

// -- CPU accounting (Z_SYS_CPU_STAT): who used the cycles between a
// cpu_snap() and a cpu_report(), as a share of the time between them.

static z_cpu_stat_args_t g_cpu0;
static int g_cpu_ok;

static void cpu_snap(void) {
	g_cpu_ok = z_cpu_stat(&g_cpu0) == Z_OK;
}

static uint64_t c64(z_cpu_count_t c) { return ((uint64_t)c.hi << 32) | c.lo; }

static void pr_pct(const char *what, uint64_t part, uint64_t whole) {
	uint32_t pm = whole ? (uint32_t)(part * 1000u / whole) : 0;
	if (pm >= 5) printf(" %s %lu.%lu%%", what, (unsigned long)(pm / 10), (unsigned long)(pm % 10));
}

// Two lines, so neither runs off an 80-column terminal: the processes,
// then the kernel's interrupts ("kind count x average cycles") and idle.
static void cpu_report(const char *label) {
	static z_cpu_stat_args_t b;
	static z_proc_info_t info[32];
	static const char *const kind[4] = { "tick", "usb", "switch", "irq" };
	if (!g_cpu_ok || z_cpu_stat(&b) != Z_OK) {
		printf("%s (no Z_SYS_CPU_STAT in this kernel)\n", label);
		return;
	}
	uint64_t whole = (uint32_t)(b.now - g_cpu0.now);
	uint32_t ni = z_proc_list(info, 32, NULL);
	printf("%s\n    procs: ", label);
	for (uint32_t p = 0; p < b.n; p++) {
		uint64_t d = c64(b.proc[p]) - c64(g_cpu0.proc[p]);
		const char *nm = p == 0 ? "sh" : "?";
		for (uint32_t i = 0; i < ni; i++) if (info[i].pid == p) nm = info[i].name;
		pr_pct(nm, d, whole);
	}
	printf("\n    kernel:");
	for (int k = 0; k < 4; k++) {
		char l[32];
		uint32_t n = b.irq_n[k] - g_cpu0.irq_n[k];
		uint64_t d = c64(b.irq[k]) - c64(g_cpu0.irq[k]);
		snprintf(l, sizeof l, "%s(%lux%lu)", kind[k], (unsigned long)n,
			(unsigned long)(n ? d / n : 0));
		pr_pct(l, d, whole);
	}
	pr_pct("idle", c64(b.idle) - c64(g_cpu0.idle), whole);
	printf("\n");
}

// What the rest of the system takes from a process that never blocks:
// spin reading the cycle counter for half a second and add up the gaps.
// Run before any worker exists, so what is measured is the timer
// interrupt and the processes that were already running (wm, net,
// console). The shell's `tick` gives the handler's own cost.
static void bench_stolen(void) {
	static const uint32_t edge[3] = { 4000, 12000, 40000 };
	static const char *const what[4] = {
		"under 4K cycles", "4K-12K", "12K-40K", "40K and over",
	};
	uint32_t cnt[4] = {0}, sum[4] = {0};
	const uint32_t span = Z_SYSCLK_HZ / 2;
	uint32_t start = z_cycles(), last = start, stolen = 0, gaps = 0;

	cpu_snap();

	printf("\n[7] time taken from a process that never blocks (0.5 s)\n");
	for (;;) {
		uint32_t now = z_cycles();
		uint32_t d = now - last;
		last = now;
		if (d > 600) {
			int b = 0;
			while (b < 3 && d >= edge[b]) b++;
			cnt[b]++;
			sum[b] += d;
			stolen += d;
			gaps++;
		}
		if (now - start >= span) break;
	}
	uint32_t pm = (uint32_t)(((uint64_t)stolen * 1000u) / span);
	printf("  %lu.%lu%% taken, in %lu gaps (%lu a second)\n",
		(unsigned long)(pm / 10), (unsigned long)(pm % 10),
		(unsigned long)gaps, (unsigned long)(gaps * 2));
	for (int b = 0; b < 4; b++) {
		if (!cnt[b]) continue;
		printf("  %-16s %5lu gaps, avg %6lu cycles\n", what[b],
			(unsigned long)cnt[b], (unsigned long)(sum[b] / cnt[b]));
	}
	cpu_report("  where the cycles went:");
}

// Both ends of a round trip receive the same way.
static void set_style(uint32_t echo, int style) {
	z_msg_t m;
	g_recv_style = style && g_has_recv;
	send(echo, SUBJ_STYLE, (uint32_t)g_recv_style);
	recv_subj(&m, SUBJ_PONG);
}

static void bench_grant(uint32_t echo) {
	z_msg_t m;
	printf("\n[5] grant window: another process writes this one's buffer\n");
	// Every copy of ipcbench is a driver (APP_DRIVER in its Makefile),
	// so the echo may open the window onto this one once it allows
	// drivers -- which the file library will do for an ordinary app.
	if (z_grant_drivers(true) != Z_OK) {
		printf("  not available (MPU without a grant window, or an older kernel)\n");
		return;
	}
	char *buf = malloc(CHUNK);
	if (!buf) return;
	memset(buf, 0, CHUNK);
	send(echo, SUBJ_GBUF, (uint32_t)(uintptr_t)buf);
	recv_subj(&m, SUBJ_PONG);

	stat_t s; st_init(&s);
	uint32_t ok = 1;
	for (int i = 0; i < 20 && ok; i++) {
		uint32_t t0 = z_cycles();
		send(echo, SUBJ_GFILL, 0);
		recv_subj(&m, SUBJ_PONG);
		st_add(&s, z_cycles() - t0);
		ok = m.obj.val.uint32;
	}
	int good = ok;
	for (int i = 0; i < CHUNK && good; i++) good = (buf[i] == (char)(i * 7 + 3));
	st_print("8KB written by another process", &s);
	printf("  content %s\n", good ? "correct" : "WRONG");

	// drivers no longer allowed: the echo's open must now fail -- and
	// it must not have written, or the MPU would have ended it
	z_grant_drivers(false);
	send(echo, SUBJ_GFILL, 0);
	recv_subj(&m, SUBJ_PONG);
	printf("  after z_grant_drivers(false): open %s\n",
		m.obj.val.uint32 ? "SUCCEEDED (wrong)" : "refused (right)");

	z_grant_drivers(true);
	g_grant = good;
	free(buf);
}

// wm's dock asks which of ~30 programs exist at startup (sw/apps/wm).
static void bench_exists(void) {
	static const char *const names[] = {
		"term", "files", "text", "read", "web", "sheet", "draw", "view",
		"track", "midi", "play", "calc", "clock", "cal", "info", "settings",
		"keyboard", "ask", "irc", "hex", "mmod", "casino", "chess",
		"kidgames", "basic", "chip8", "space3d", "gpu3d", "mausrun",
	};
	const uint32_t n = sizeof(names) / sizeof(names[0]);
	uint8_t found[sizeof(names) / sizeof(names[0])];
	uint32_t hits = 0;

	printf("\n[6] which of %lu programs exist (wm's dock at startup)\n", (unsigned long)n);
	uint32_t t0 = z_cycles();
	for (uint32_t i = 0; i < n; i++) hits += z_exec_exists(names[i]) ? 1 : 0;
	uint32_t one = z_cycles() - t0;
	printf("  one by one: %lu found in ", (unsigned long)hits); pr_us(one); printf(" us\n");

	t0 = z_cycles();
	if (z_exec_exists_many(names, n, found) != Z_OK) {
		printf("  batched: not available (older kernel)\n");
		return;
	}
	uint32_t many = z_cycles() - t0, h2 = 0;
	for (uint32_t i = 0; i < n; i++) h2 += found[i];
	printf("  batched:    %lu found in ", (unsigned long)h2); pr_us(many); printf(" us%s\n",
		h2 == hits ? "" : "  (DIFFERENT answer)");
}

static int master(void) {
	uint32_t spin[N_SPIN] = {0};

	printf("\nipcbench -- docs/userspace_drivers.md, phase 0\n");
	printf("clock %lu Hz, %lu-cycle ticks\n",
		(unsigned long)Z_SYSCLK_HZ, (unsigned long)65536u);
	g_has_recv = probe_recv();
	printf("z_msg_recv(): %s\n", g_has_recv ? "yes" : "no (older kernel)");

	bench_cpu();
	bench_stolen();

	uint32_t echo = spawn(ROLE_ECHO);
	uint32_t sl = echo ? spawn(ROLE_SLEEP) : 0;
	if (!echo || !sl) return 1;

	printf("\n[2] message round trip (send PING, block, PONG back)\n");
	// warm up: the first trips fault in caches and the echo's first wait
	for (int i = 0; i < 20; i++) { z_msg_t m; send(echo, SUBJ_PING, 0); recv_subj(&m, SUBJ_PONG); }
	set_style(echo, 0);
	round_trips(echo, "idle, read/wait/read");
	if (g_has_recv) {
		set_style(echo, 1);
		round_trips(echo, "idle, z_msg_recv()");
	}
	if (ktrace(1)) {
		for (int i = 0; i < 4; i++) { z_msg_t m; send(echo, SUBJ_PING, 0); recv_subj(&m, SUBJ_PONG); }
		ktrace(0);
		printf("  (4 round trips traced: `ktrace` in the shell prints them)\n");
	}
	for (int k = 0; k < N_SPIN; k++) {
		spin[k] = spawn(ROLE_SPIN);
		char l[48];
		snprintf(l, sizeof l, "with %d CPU-bound process%s", k + 1, k ? "es" : "");
		round_trips(echo, l);
	}
	for (int k = 0; k < N_SPIN; k++) if (spin[k]) send(spin[k], SUBJ_STOP, 0);
	z_proc_wait(8);

	printf("\n[3] wakeup latency: z_proc_wait(1) loop, longest gap\n");
	sleeper_reset(sl);
	z_proc_wait(400);   // ~0.5s of nothing
	stall_report(sl, "idle system");

	bench_grant(echo);

	// Every file is fs's now (docs/fs_driver.md); this program links
	// zfsc.o, so these go to fs directly, as an app built that way would.
	printf("\n[4] filesystem (through fs)\n");
	bench_fs_small(Z_DIR_RAM "/ipcb_s.tmp", Z_DIR_RAM);
	bench_fs_small("/ipcb_s.tmp", "card");
	bench_fs_bulk(Z_DIR_RAM "/ipcb.tmp", Z_DIR_RAM, sl, echo);
	bench_fs_bulk("/ipcb.tmp", "card", sl, echo);
	// USB storage, through `usb` (docs/usb_driver.md), when a drive is
	// mounted. A sector is one SCSI command, so this is the slowest.
	{
		z_fs_info_t fi;
		if (zfs_stat(Z_DIR_USB, &fi) == 0) {
			bench_fs_small(Z_DIR_USB "/ipcb_s.tmp", Z_DIR_USB);
			bench_fs_bulk(Z_DIR_USB "/ipcb.tmp", Z_DIR_USB, sl, echo);
		} else {
			printf("  (no drive at " Z_DIR_USB ": the USB storage part skipped)\n");
		}
	}

	bench_exists();

	send(echo, SUBJ_STOP, 0);
	send(sl, SUBJ_STOP, 0);
	printf("\nipcbench: done\n");
	return 0;
}

int main(void) {
	// The master registers first; a copy that finds it is a worker.
	uint32_t pid;
	if (z_pid_lookup("ipcb0", &pid) && pid != z_getpid()) {
		g_master = pid;
		z_pid_register("ipcbw", g_name, sizeof g_name);
		return worker();
	}
	if (!z_pid_register("ipcb", g_name, sizeof g_name)) {
		printf("ipcbench: cannot register\n");
		return 1;
	}
	return master();
}
