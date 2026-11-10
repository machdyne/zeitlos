/*
 * Host test and render for sw/apps/checkup: the REAL checkup.c, with a
 * scripted kernel -- the memory pool, an arena, the key/value store, a
 * card and a RAM disk, the console log -- every check run to the end,
 * failures made on purpose, and the report saved.
 *
 *   sudo sysctl -w vm.mmap_min_addr=0      # see sw/common/tests/ztramp.h
 *   make -C sw/apps/checkup test
 *
 * Exit 0 pass, 1 fail, 77 skipped. docs/checkup.md.
 *
 * No TRNG or RTC on the build machine (the CSR page reads 0), so those
 * two report skip here; on a board they run.
 */

#include "../../../common/tests/zrender.h"

#include <stdlib.h>
#include "zfs.h"
#include "zproc.h"
#include "zkv.h"
#include "zconsole.h"

// -- the save dialog, scripted --
static const char *save_answer;		// NULL: Cancel
static char save_dir[64];
static int confirm_answer = 1, confirms;

#define main checkup_main_unused
#include "../checkup.c"
#undef main

bool z_dialog_save(const z_dialog_ctx_t *c, const char *start_dir, const char *suggested,
	char *out, int outlen) {
	(void)c; (void)suggested;
	snprintf(save_dir, sizeof(save_dir), "%s", start_dir);
	if (!save_answer) return false;
	snprintf(out, (size_t)outlen, "%s", save_answer);
	return true;
}
int z_dialog_confirm(const z_dialog_ctx_t *c, const char *t, const char *m, int b) {
	(void)c; (void)t; (void)m; (void)b;
	confirms++;
	return confirm_answer ? Z_DIALOG_YES : Z_DIALOG_NO;
}
void z_dialog_help(const z_dialog_ctx_t *c, const char *t, const char *x, const char *d) {
	(void)c; (void)t; (void)x; (void)d;
}

// -- a scripted kernel --

#define ARENA_ADDR 0x30000000UL
#define ARENA_MAX  (512u * 1024)

static bool card_in = true, ramdisk = true, usb_in, net_up = true, kv_ok = true, mem_ok = true;
static int corrupt_at = -1;		// a word of the arena that will not hold its value
static uint32_t arena_now;
static uint32_t ticks = 10000;
static char klog_text[Z_KLOG_SIZE];
static uint32_t klog_len;

static const char manifest[] = "name v0.1.0\nf 9d41c2a07b55 8e0f61d2 1734 k base sys/zeitlos.cfg\n";
static bool manifest_open;
static uint32_t manifest_pos;

// files on the RAM disk and the USB stick, one each is enough
static struct { char name[64]; char data[16384]; int len; } files[3];

static int file_find(const char *name) {
	for (int i = 0; i < 3; i++) if (files[i].len >= 0 && !strcmp(files[i].name, name)) return i;
	return -1;
}

static bool writable(const char *name) {
	if (!strncmp(name, Z_DIR_TMP "/", 5) || !strncmp(name, Z_DIR_RAM "/", 5)) return ramdisk;
	if (!strncmp(name, Z_DIR_USB "/", 5)) return usb_in;
	return card_in;
}

static z_obj_t k_ok, k_fail;

static uint32_t *k_syscall(uint32_t id, uint32_t *args, uint32_t b) {

	(void)b;

	switch (id) {

	case Z_SYS_MSG_READ:
		return (uint32_t *)&k_fail;

	case Z_SYS_UPTIME:
		ticks += 40;	// time passes as the app asks for it
		((z_obj_t *)args)->type = Z_UINT32;
		((z_obj_t *)args)->val.uint32 = ticks;
		return (uint32_t *)&k_ok;

	case Z_SYS_MEM_STATS: {
		if (!mem_ok) return (uint32_t *)&k_fail;
		z_mem_stats_args_t *m = (z_mem_stats_args_t *)args;
		memset(m, 0, sizeof(*m));
		m->total = 24u * 1024 * 1024;
		m->free = 20u * 1024 * 1024;
		m->used = m->total - m->free;
		m->largest_free = 16u * 1024 * 1024;
		return (uint32_t *)&k_ok;
	}

	case Z_SYS_ARENA: {
		z_arena_args_t *a = (z_arena_args_t *)args;
		if (a->size != Z_ARENA_QUERY) arena_now = a->size > ARENA_MAX ? 0 : a->size;
		a->size = arena_now;
		a->base = arena_now ? (uint32_t)ARENA_ADDR : 0;
		a->result = Z_ARENA_OK;
		return (uint32_t *)&k_ok;
	}

	case Z_SYS_KV: {
		z_kv_args_t *a = (z_kv_args_t *)args;
		if (!kv_ok) { a->result = Z_KV_E_NOSYS; break; }
		if (a->op == Z_KV_INFO) {
			z_kv_info_t *ki = (z_kv_info_t *)a->val;
			memset(ki, 0, sizeof(*ki));
			ki->size = 4096; ki->flags = Z_KV_INFO_WRITABLE; ki->live = 312; ki->keys = 3;
			a->result = Z_KV_OK;
		} else a->result = Z_KV_E_NOENT;
		break;
	}

	case Z_SYS_FS_DF: {
		z_fs_df_args_t *a = (z_fs_df_args_t *)args;
		a->total_kb = card_in ? 7774208u : 0;
		a->free_kb = card_in ? 7000000u : 0;
		return (uint32_t *)&k_ok;
	}

	case Z_SYS_FS_STAT: {
		z_fs_stat_args_t *a = (z_fs_stat_args_t *)args;
		if (!strcmp(a->name, Z_DIR_USB) && usb_in) return (uint32_t *)&k_ok;
		return (uint32_t *)&k_fail;
	}

	case Z_SYS_FS_SIZE: {
		z_fs_size_args_t *a = (z_fs_size_args_t *)args;
		int i = file_find(a->name);
		a->size = 0;
		if (card_in && !strcmp(a->name, Z_PATH_SYS_MANIFEST)) a->size = sizeof(manifest) - 1;
		else if (i >= 0) a->size = (uint32_t)files[i].len;
		return (uint32_t *)&k_ok;
	}

	case Z_SYS_FS_READ: {
		z_fs_read_args_t *a = (z_fs_read_args_t *)args;
		int i = file_find(a->name);
		a->len = 0;
		if (i >= 0) {
			a->len = (uint32_t)files[i].len < a->maxlen ? (uint32_t)files[i].len : a->maxlen;
			memcpy(a->buf, files[i].data, a->len);
		}
		return (uint32_t *)&k_ok;
	}

	case Z_SYS_FS_WRITE: {
		z_fs_write_args_t *a = (z_fs_write_args_t *)args;
		a->written = 0;
		if (!writable(a->name) || a->len > sizeof(files[0].data)) return (uint32_t *)&k_ok;
		int i = file_find(a->name);
		for (int k = 0; i < 0 && k < 3; k++) if (files[k].len < 0) i = k;
		if (i < 0) return (uint32_t *)&k_ok;
		snprintf(files[i].name, sizeof(files[i].name), "%s", a->name);
		memcpy(files[i].data, a->buf, a->len);
		files[i].len = (int)a->len;
		a->written = a->len;
		return (uint32_t *)&k_ok;
	}

	case Z_SYS_FS_UNLINK: {
		z_fs_unlink_args_t *a = (z_fs_unlink_args_t *)args;
		int i = file_find(a->name);
		if (i < 0) return (uint32_t *)&k_fail;
		files[i].len = -1;
		return (uint32_t *)&k_ok;
	}

	case Z_SYS_FS_OPEN_READ: {
		z_fs_open_args_t *a = (z_fs_open_args_t *)args;
		if (!card_in || strcmp(a->name, Z_PATH_SYS_MANIFEST)) return (uint32_t *)&k_fail;
		manifest_open = true;
		manifest_pos = 0;
		a->handle = 3;
		return (uint32_t *)&k_ok;
	}

	case Z_SYS_FS_READ_CHUNK: {
		z_fs_read_chunk_args_t *a = (z_fs_read_chunk_args_t *)args;
		uint32_t left = sizeof(manifest) - 1 - manifest_pos;
		a->len = left < a->maxlen ? left : a->maxlen;
		if (a->len > 16) a->len = 16;			// small chunks: several steps
		memcpy(a->buf, manifest + manifest_pos, a->len);
		manifest_pos += a->len;
		return (uint32_t *)&k_ok;
	}

	case Z_SYS_FS_CLOSE:
		manifest_open = false;
		return (uint32_t *)&k_ok;

	case Z_SYS_PID_LOOKUP: {
		z_obj_t *o = (z_obj_t *)args;
		if (net_up && o->type == Z_STR && !strcmp(o->val.str, "net0")) {
			o->type = Z_UINT32;
			o->val.uint32 = 2;
			return (uint32_t *)&k_ok;
		}
		return (uint32_t *)&k_fail;
	}

	case Z_SYS_KLOG_READ: {
		z_klog_args_t *a = (z_klog_args_t *)args;
		uint32_t left = a->pos < klog_len ? klog_len - a->pos : 0;
		a->n = left < a->len ? left : a->len;
		if (a->n > 128) a->n = 128;
		memcpy(a->buf, klog_text + a->pos, a->n);
		a->pos += a->n;
		a->lost = 0;
		return (uint32_t *)&k_ok;
	}

	case Z_SYS_ABI: {
		z_abi_args_t *a = (z_abi_args_t *)args;
		snprintf((char *)a->id, sizeof(a->id), "1c6b6ccabdf8b075");
		return (uint32_t *)&k_ok;
	}

	default:
		break;

	}

	return (uint32_t *)&k_ok;

}

static bool k_install(void) {
	if ((uintptr_t)(void *)k_syscall > 0xFFFFFFFFu) return false;
	if (mmap((void *)0, 4096, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == MAP_FAILED)
		return false;
	if (mmap((void *)0x70000000UL, 4096, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == MAP_FAILED)
		return false;
	if (mmap((void *)ARENA_ADDR, ARENA_MAX, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == MAP_FAILED)
		return false;
	k_ok.type = Z_UINT32; k_ok.val.uint32 = Z_OK;
	k_fail.type = Z_UINT32; k_fail.val.uint32 = Z_FAIL;
	*(volatile uint32_t *)0x0000000c = (uint32_t)(uintptr_t)k_syscall;
	return true;
}

// -- checks --

static int checks, failures;

static void expect(bool ok, const char *what) {
	checks++;
	if (!ok) { failures++; printf("  FAIL: %s\n", what); }
}

static void render(const char *prefix, const char *name) {
	char path[256];
	z_render_clear();
	repaint();
	snprintf(path, sizeof(path), "%s-%s.pbm", prefix, name);
	z_render_write(path, &win, 2);
}

// Run every check to the end, as the main loop would, counting steps.
// A memory word that will not hold its value, made on purpose.
static int run_all(void) {
	int steps = 0;
	start_run();
	while (step()) {
		steps++;
		if (corrupt_at >= 0 && cur == C_MEMORY && (phase == 2 || phase == 4))
			((volatile uint32_t *)ARENA_ADDR)[corrupt_at] ^= 1;
		if (steps > 100000) break;
	}
	return steps;
}

#define CRASH "*** view (pid 7) crashed: store outside its own memory\n" \
	"    address 4001f2c0 (kernel memory)  at pc 800012a4\n" \
	"    ended; the rest of the system is unaffected\n"

int main(int argc, char **argv) {

	const char *prefix = argc > 1 ? argv[1] : "/tmp/checkup";

	if (!z_render_open(&win, WIN_W, WIN_H) || !k_install()) {
		printf("checkup render: skipped (cannot map fixed addresses)\n");
		return 77;
	}
	for (int i = 0; i < 3; i++) files[i].len = -1;

	widgets_init();
	layout();

	// -- 1. a healthy machine --
	printf("1. everything there\n");
	klog_len = (uint32_t)snprintf(klog_text, sizeof(klog_text), "init: done\nwm: hello\n");
	int steps = run_all();
	expect(cur == C_COUNT, "every check ran to the end");
	expect(steps > 20, "in many steps, not one long call");
	expect(res[C_SYSTEM].st == ST_PASS && strstr(res[C_SYSTEM].info, Z_OS_VERSION) != NULL, "System: the release");
	expect(res[C_MEMORY].st == ST_PASS && strstr(res[C_MEMORY].info, "256KB") != NULL,
		"Memory: 256KB borrowed, written and read back");
	expect(arena_now == 0, "and the arena given back");
	expect(res[C_FLASH].st == ST_PASS && strstr(res[C_FLASH].info, "3 keys") != NULL, "Flash store: answers");
	expect(res[C_RANDOM].st == ST_SKIP && res[C_CLOCK].st == ST_SKIP, "no TRNG or RTC here: skipped");
	expect(res[C_CARD].st == ST_PASS && !manifest_open, "Memory card: the manifest read to the end, and closed");
	expect(res[C_RAMDISK].st == ST_PASS && file_find(RAM_TEST_PATH) < 0, "RAM disk: written, read back, removed");
	expect(res[C_NET].st == ST_PASS, "Network: running");
	expect(res[C_CRASH].st == ST_PASS, "Crashes: none in the log");
	render(prefix, "1-healthy");

	// -- 2. the report: saved where the person chooses --
	usb_in = true;
	save_answer = Z_DIR_USB "/checkup.txt";
	save_report();
	int i = file_find(Z_DIR_USB "/checkup.txt");
	expect(!strcmp(save_dir, Z_DIR_USB), "the save dialog starts on the USB stick when there is one");
	expect(i >= 0 && strstr(files[i].data, "nothing failed") && strstr(files[i].data, "Memory card  pass")
		&& strstr(files[i].data, "The console log:\ninit: done"), "the report: results, then the log");
	expect(strstr(status, "saved") != NULL, "and the status line says so");
	int before = confirms;
	confirm_answer = 0;
	save_report();
	expect(confirms == before + 1 && strstr(status, "saved") != NULL, "a file already there: asked, and kept on No");
	confirm_answer = 1;
	save_answer = NULL;
	usb_in = false;
	files[i].len = -1;
	save_answer = Z_DIR_RAM "/checkup.txt";
	save_report();
	expect(!strcmp(save_dir, Z_DIR_RAM), "no USB stick: the dialog starts on the RAM disk");
	save_answer = NULL;

	// -- 3. a crash in the log, a memory word that does not hold --
	printf("2. a crash, and bad memory\n");
	klog_len = (uint32_t)snprintf(klog_text, sizeof(klog_text), "boot\n" CRASH "wm: after\n");
	corrupt_at = 1000;
	run_all();
	corrupt_at = -1;
	expect(res[C_MEMORY].st == ST_FAIL && strstr(res[C_MEMORY].info, "at 30000fa0") != NULL,
		"Memory: the bad word found, by address");
	expect(arena_now == 0, "and the arena given back even so");
	expect(res[C_CRASH].st == ST_NOTE && !strncmp(res[C_CRASH].info, "view (pid 7) crashed:", 21),
		"Crashes: the last one, named");
	render(prefix, "2-crash-and-memory");
	save_answer = Z_DIR_RAM "/c2.txt";
	save_report();
	i = file_find(Z_DIR_RAM "/c2.txt");
	expect(i >= 0 && strstr(files[i].data, "something failed") && strstr(files[i].data, "The last crash:\n" CRASH),
		"the report has the failure and the whole crash report");
	save_answer = NULL;

	// -- 4. no card, no RAM disk, no network, an old kernel --
	printf("3. a bare machine\n");
	card_in = false; ramdisk = false; net_up = false; kv_ok = false; mem_ok = false;
	klog_len = 0;
	for (int k = 0; k < 3; k++) files[k].len = -1;
	run_all();
	expect(res[C_MEMORY].st == ST_SKIP && res[C_FLASH].st == ST_SKIP && res[C_CARD].st == ST_SKIP &&
		res[C_RAMDISK].st == ST_SKIP && res[C_NET].st == ST_NOTE, "what is not there is skipped, not failed");
	render(prefix, "3-bare");

	// -- the window --
	{
		bool inside = true;
		int cw = z_win_content_w(&win), ch = z_win_content_h(&win);
		for (int b = 0; b < B_COUNT; b++)
			if (buttons[b].x < 0 || buttons[b].x + buttons[b].w > cw || buttons[b].y + buttons[b].h > ch) inside = false;
		expect(inside && rows_y() + C_COUNT * LINE_H < btn_y() - 7, "the rows and the buttons inside the window");
	}
	handle_key(Z_KEY_F5, 0);
	expect(cur == 0 && res[C_SYSTEM].st == ST_RUN, "F5: run again, from the top");
	handle_key('\t', 0);
	expect(bset.focused == B_CLOSE, "Tab: Save report, then Close");
	handle_key(0x0d, 0);
	expect(finished, "Enter on Close closes it");

	printf("checkup render: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;

}
