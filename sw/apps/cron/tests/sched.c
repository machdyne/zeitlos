/*
 * Host test for sw/apps/cron -- the real cron.c, with a fake clock, a
 * fake uptime, an in-memory filesystem and recorded program starts.
 *
 *   cc -std=gnu99 -Wall -I sw/common -o /tmp/cron_sched \
 *      sw/apps/cron/tests/sched.c sw/common/zrtc.c
 *   /tmp/cron_sched
 *
 * See docs/cron.md.
 */

#define CRON_HOST_TEST 1

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

static bool fake_valid;
static uint32_t fake_utc;
static bool clock_valid(void) { return fake_valid; }
static uint32_t clock_now(void) { return fake_utc; }

#include "../cron.c"

// -- stubs --

static char console[8192];
static int console_n;
void uart_putc(char c) { if (console_n < (int)sizeof(console) - 1) console[console_n++] = c; }

static uint32_t fake_ticks;
uint32_t z_uptime_ticks(void) { return fake_ticks; }
void z_proc_wait(uint32_t t) { fake_ticks += t; }
z_rv z_msg_read(z_msg_t *m) { (void)m; return Z_FAIL; }
bool z_pid_register(const char *b, char *out, uint32_t n) { snprintf(out, n, "%s0", b); return true; }

static char tzsetting[40] = "UTC";
static uint32_t cfg_generation_now = 1;
bool z_cfg_get(const char *key, char *out, size_t n) {
	if (!strcmp(key, "system.rtc.timezone")) { snprintf(out, n, "%s", tzsetting); return true; }
	return false;
}
uint32_t z_cfg_generation(void) { return cfg_generation_now; }

static char started[64][200];
static int nstart;
static char pending_arg[200];
static bool pid_running[256];
static uint32_t next_pid = 10;
static bool refuse_start;
void z_launch_arg_set(const char *a) { snprintf(pending_arg, sizeof(pending_arg), "%s", a); }
uint32_t z_proc_run(const char *name) {
	if (refuse_start) return 0;
	if (nstart < 64) snprintf(started[nstart++], 200, "%s|%s", name, pending_arg);
	pid_running[next_pid] = true;
	return next_pid++;
}
z_rv z_proc_status(uint32_t pid, uint32_t *state, int32_t *status) {
	if (state) *state = pid_running[pid] ? Z_PROC_STATE_RUNNING : Z_PROC_STATE_EXITED;
	if (status) *status = 0;
	return Z_OK;
}

// A tiny filesystem: a few named files.
typedef struct { char name[40]; char data[20000]; int size; bool used; } mf_t;
static mf_t files[6];
static mf_t *fopen_(const char *n, bool create) {
	for (int i = 0; i < 6; i++) if (files[i].used && !strcmp(files[i].name, n)) return &files[i];
	if (!create) return NULL;
	for (int i = 0; i < 6; i++) if (!files[i].used) {
		files[i].used = true; snprintf(files[i].name, 40, "%s", n); files[i].size = 0; return &files[i];
	}
	return NULL;
}
static mf_t *handle_file; static int handle_pos;
int fs_size(char *n) { mf_t *f = fopen_(n, false); return f ? f->size : -1; }
int fs_read_file(char *n, char *buf, int max) {
	mf_t *f = fopen_(n, false); if (!f) return -1;
	int k = f->size < max ? f->size : max; memcpy(buf, f->data, (size_t)k); return k;
}
int fs_write_file(char *n, char *buf, int len) {
	mf_t *f = fopen_(n, true); memcpy(f->data, buf, (size_t)len); f->size = len; return len;
}
int fs_open_write(const char *n) { handle_file = fopen_(n, true); handle_file->size = 0; handle_pos = 0; return 1; }
int fs_open_rw(const char *n) { handle_file = fopen_(n, false); handle_pos = 0; return handle_file ? 1 : -1; }
int fs_seek(int h, uint32_t off) { (void)h; handle_pos = (int)off; return 0; }
int fs_write_chunk(int h, const void *b, int len) {
	(void)h;
	memcpy(handle_file->data + handle_pos, b, (size_t)len);
	handle_pos += len;
	if (handle_pos > handle_file->size) handle_file->size = handle_pos;
	return len;
}
int fs_close_handle(int h) { (void)h; return 1; }
int fs_mkdir(const char *p) { (void)p; return 1; }

// -- helpers --

static int checks, failures;
static void expect(bool ok, const char *what) {
	checks++;
	if (!ok) { failures++; printf("  FAIL: %s\n", what); }
}

// UTC seconds for a date and time.
static uint32_t at(int y, int mo, int d, int h, int mi) {
	z_tm_t tm = { y, (uint8_t)mo, (uint8_t)d, (uint8_t)h, (uint8_t)mi, 0, 0, 0 };
	return z_tm_to_time(&tm);
}

static void config(const char *text) {
	mf_t *f = fopen_(CFG_PATH, true);
	snprintf(f->data, sizeof(f->data), "%s", text);
	f->size = (int)strlen(text);
}

static void reset(void) {
	memset(files, 0, sizeof(files));
	memset(jobs, 0, sizeof(jobs));
	njobs = 0; nstart = 0; console_n = 0; console[0] = 0;
	memset(pid_running, 0, sizeof(pid_running));
	fake_ticks = 0; up_last_ticks = up_frac = up_secs = 0;
	last_minute = 0; refuse_start = false; next_pid = 10;
	snprintf(tzsetting, sizeof(tzsetting), "UTC");
}

// Runs cron's passes from the current fake time up to `until`, a minute
// at a time (as it would wake), advancing uptime with it.
static void run_until(uint32_t until) {
	while (fake_utc < until) {
		cron_step();
		fake_utc += 60;
		fake_ticks += 60 * Z_TICK_HZ;
	}
	cron_step();
}

static int count_started(const char *prog) {
	int n = 0;
	for (int i = 0; i < nstart; i++)
		if (!strncmp(started[i], prog, strlen(prog)) && started[i][strlen(prog)] == '|') n++;
	return n;
}

int main(void) {

	// -- parsing --
	reset();
	config("# comment\n"
		"wait_for_ntp: no\n"
		"daily 03:00 backup /docs\n"
		"weekly MON 02:30 backup --full\n"
		"monthly 1 04:00 tidy\n"
		"hourly :15 sync\n"
		"every 30m check-mail\n"
		"at boot mount-usb\n"
		"daily 25:00 nope\n"
		"sometimes 03:00 nope\n"
		"daily 03:00\n"
		"daily 05:00 text 'My Notes.txt'\n");
	expect(load_config(), "config reads");
	expect(!wait_for_ntp, "wait_for_ntp: no");
	expect(njobs == 7, "seven good rules, three bad ones reported");
	expect(strstr(console, "line 9") && strstr(console, "line 10") && strstr(console, "line 11"),
		"bad lines named by number");
	expect(jobs[0].kind == J_DAILY && jobs[0].hour == 3 && jobs[0].min == 0 &&
		!strcmp(jobs[0].prog, "backup") && !strcmp(jobs[0].args, "/docs"), "daily rule");
	expect(jobs[1].kind == J_WEEKLY && jobs[1].wday == 1, "weekly MON (any case)");
	expect(jobs[4].kind == J_EVERY && jobs[4].every == 1800, "every 30m");
	expect(!strcmp(jobs[6].args, "'My Notes.txt'"), "an argument with a space is passed quoted");

	// -- daily --
	reset();
	config("daily 03:00 backup /docs\n");
	load_config();
	fake_valid = true;
	fake_utc = at(2026, 9, 25, 2, 58);
	cron_begin();
	run_until(at(2026, 9, 25, 2, 59));
	expect(count_started("backup") == 0, "not before 03:00");
	run_until(at(2026, 9, 25, 3, 5));
	expect(count_started("backup") == 1, "once at 03:00");
	expect(!strcmp(started[0], "backup|/docs"), "with its argument");
	pid_running[10] = false;
	run_until(at(2026, 9, 26, 3, 5));
	expect(count_started("backup") == 2, "and again the next day");
	expect(strstr(files[1].data, "started backup /docs") || fopen_(LOG_PATH, false), "logged");
	expect(strstr(fopen_(LOG_PATH, false)->data, "exited 0: backup /docs") != NULL, "its exit logged");

	// -- a job still running is not started twice --
	reset();
	config("every 1m slow\n");
	load_config();
	fake_valid = true;
	fake_utc = at(2026, 9, 25, 12, 0);
	cron_begin();
	run_until(at(2026, 9, 25, 12, 5));
	expect(count_started("slow") == 1, "every 1m: the first run is still going, so no more");
	expect(strstr(fopen_(LOG_PATH, false)->data, "skipped, still running: slow") != NULL, "skips logged");
	pid_running[10] = false;
	run_until(at(2026, 9, 25, 12, 7));
	expect(count_started("slow") >= 2, "once it exits, it runs again");

	// -- catching up --
	reset();
	config("daily 03:00 backup\nhourly :15 sync\n");
	load_config();
	jobs[0].last_run = at(2026, 9, 22, 3, 0);		// three days ago
	jobs[1].last_run = at(2026, 9, 22, 3, 15);
	fake_valid = true;
	fake_utc = at(2026, 9, 25, 10, 0);
	cron_begin();
	run_until(at(2026, 9, 25, 10, 5));
	expect(count_started("backup") == 1, "a missed daily job catches up once");
	expect(count_started("sync") == 0, "an hourly one does not");
	run_until(at(2026, 9, 25, 11, 20));
	expect(count_started("backup") == 1, "caught up only once");
	expect(count_started("sync") == 1, "hourly at :15");

	// -- state survives a restart --
	reset();
	config("daily 03:00 backup\n");
	load_config();
	fake_valid = true;
	fake_utc = at(2026, 9, 25, 3, 0);
	cron_begin();
	run_until(at(2026, 9, 25, 3, 1));
	expect(fopen_(STATE_PATH, false) != NULL, "state written");
	uint32_t ran = jobs[0].last_run;
	memset(jobs, 0, sizeof(jobs));
	njobs = 0;
	load_config();
	state_load();
	expect(jobs[0].last_run == ran, "last run read back");

	// -- weekly, monthly --
	reset();
	config("weekly mon 02:30 w\nmonthly 1 04:00 m\n");
	load_config();
	fake_valid = true;
	fake_utc = at(2026, 9, 27, 0, 0);				// a Sunday
	cron_begin();
	run_until(at(2026, 10, 2, 0, 0));
	expect(count_started("w") == 1, "weekly: Monday only");
	expect(count_started("m") == 1, "monthly: the 1st");

	// -- time zones and summer time --
	reset();
	snprintf(tzsetting, sizeof(tzsetting), "Berlin");
	config("daily 03:00 b\n");
	load_config();
	fake_valid = true;
	fake_utc = at(2026, 7, 1, 0, 0);
	cron_begin();
	run_until(at(2026, 7, 1, 0, 59));
	expect(count_started("b") == 0, "not at 03:00 UTC...");
	run_until(at(2026, 7, 1, 1, 1));
	expect(count_started("b") == 1, "...but at 01:00 UTC, 03:00 in Berlin summer time");

	// The hour repeated when summer time ends: 2026-10-25, 02:00-03:00
	// happens twice in Berlin. A job at 02:30 runs once.
	reset();
	snprintf(tzsetting, sizeof(tzsetting), "Berlin");
	config("daily 02:30 twice\n");
	load_config();
	fake_valid = true;
	fake_utc = at(2026, 10, 24, 23, 0);
	cron_begin();
	run_until(at(2026, 10, 25, 0, 31));			// 02:30 CEST
	expect(count_started("twice") == 1, "02:30, the first time");
	// Finished well before the hour repeats, so it is the summer-time
	// guard that stops a second run, not the still-running rule.
	pid_running[10] = false;
	run_until(at(2026, 10, 25, 3, 0));			// through 02:30 CET
	expect(count_started("twice") == 1, "the repeated hour runs it once");

	// -- boot and every --
	reset();
	config("at boot hello\nevery 10m tick\n");
	load_config();
	fake_valid = false;								// no clock at all
	cron_begin();
	expect(count_started("hello") == 1, "at boot runs at once");
	for (int i = 0; i < 25; i++) { fake_ticks += 60 * Z_TICK_HZ; cron_step(); pid_running[next_pid - 1] = false; }
	expect(count_started("tick") == 2, "every 10m, counted from uptime, no clock needed");

	// -- reload keeps what it knows --
	reset();
	config("daily 03:00 backup\n");
	load_config();
	jobs[0].last_run = 12345;
	config("daily 03:00 backup\nhourly :00 new\n");
	cfg_generation_now++;
	fake_valid = false;
	cron_step();
	expect(njobs == 2 && jobs[0].last_run == 12345, "cfg reload re-reads, keeping an unchanged job's history");

	// -- a program that will not start --
	reset();
	config("at boot missing\n");
	load_config();
	refuse_start = true;
	cron_begin();
	expect(strstr(fopen_(LOG_PATH, false)->data, "could not start missing") != NULL, "a failed start is logged");

	printf("cron: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;

}
