/*
 * cron -- runs programs on a schedule
 *
 * Reads /user/cron.cfg, starts programs when their time comes, and
 * sleeps in between. A core app, in flash (docs/flash_apps.md); init
 * starts it at boot when /user/cron.cfg exists. docs/cron.md.
 *
 *   # /user/cron.cfg
 *   wait_for_ntp: yes
 *
 *   daily 03:00         backup /docs
 *   weekly mon 02:30    backup --full
 *   monthly 1 04:00     tidy
 *   hourly :15          sync
 *   every 30m           check-mail
 *   at boot             mount-usb
 *
 * -- the clock --
 *
 * No board has a battery for its clock (docs/rtc.md): after power-up
 * the time is unknown until net's NTP client sets it, and a board with
 * no network may never know it. So with `wait_for_ntp: yes`, the
 * default, cron does nothing at all until the clock is valid. With `no`
 * it starts at once: `at boot` and `every` jobs need no clock, and the
 * clock-time jobs (hourly, daily, weekly, monthly) run whenever the
 * clock is valid -- set later by NTP, or never on a board that cannot
 * reach a server.
 *
 * Clock times are local: system.rtc.timezone from /zeitlos.cfg, summer
 * time included. A job in the hour skipped in spring runs when it is
 * next due or caught up (below); in the hour repeated in autumn it runs
 * once.
 *
 * -- running a job --
 *
 * A job is a program and its arguments, split with the shell's quoting
 * (sw/common/zargs.h) and passed on as `run` passes them: the arguments
 * as the launch argument, re-quoted where they need it. A job still
 * running from its last time is not started again -- that is logged.
 *
 * -- catching up --
 *
 * A daily, weekly or monthly job whose last run is more than its period
 * (plus an hour) ago runs once, as soon as cron is running and the
 * clock is valid -- the machine was off, or not yet synced, at 03:00.
 * Last runs are kept in /user/cron.state. Hourly and `every` jobs do
 * not catch up: a machine off for a day should not run 24 syncs at once.
 *
 * -- the log --
 *
 * One line per start, skip and exit in /user/cron.log, cut back to
 * nothing when it passes 16KB, and on the console.
 *
 * -- size --
 *
 * No stdio: printf alone would be most of the image, the reason jfont
 * and the keyboard app format by hand too.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"
#include "../../common/zproc.h"
#include "../../common/zfsapp.h"
#include "../../common/zcfg.h"
#include "../../common/zrtc.h"
#include "../../common/zwin.h"		// z_launch_arg_set()
#include "../../common/zargs.h"

#define CFG_PATH    "/user/cron.cfg"
#define STATE_PATH  "/user/cron.state"
#define LOG_PATH    "/user/cron.log"
#define LOG_MAX     16384

#define MAX_JOBS    32
#define CFG_MAX     4096
#define LINE_MAX    192

enum { J_BOOT, J_EVERY, J_HOURLY, J_DAILY, J_WEEKLY, J_MONTHLY };

typedef struct {
	uint8_t		kind;
	uint8_t		min, hour, wday, mday;
	uint32_t	every;			// J_EVERY: seconds
	char		prog[32];
	char		args[160];		// the launch argument, quoted as run does
	uint32_t	key;			// hash of the rule's line: its identity in the state file
	uint32_t	last_run;		// UTC seconds, 0 = never (as far as we know)
	uint32_t	next_up;		// J_EVERY: uptime second it is next due
	uint32_t	pid;			// running, or 0
	bool		boot_done;
} job_t;

static job_t jobs[MAX_JOBS];
static int njobs;
static bool wait_for_ntp = true;
static z_tz_t tz;
static uint32_t cfg_gen;

static char cfg_buf[CFG_MAX + 1];

// -- the clock, behind two calls a host test can replace --

#ifndef CRON_HOST_TEST
static bool clock_valid(void) { return z_rtc_valid(); }
static uint32_t clock_now(void) { return z_rtc_seconds(); }
#endif

// -- output, without stdio --

void uart_putc(char c);

static void say(const char *s) {
	while (*s) { if (*s == '\n') uart_putc('\r'); uart_putc(*s++); }
}

typedef struct { char *b; int n, cap; } sb_t;

static void sb_s(sb_t *sb, const char *s) {
	while (*s && sb->n < sb->cap - 1) sb->b[sb->n++] = *s++;
	sb->b[sb->n] = 0;
}

static void sb_u(sb_t *sb, uint32_t v, int width) {
	char t[11];
	int i = 10;
	t[i] = 0;
	do { t[--i] = (char)('0' + v % 10); v /= 10; } while (v && i);
	while (10 - i < width && i) t[--i] = '0';
	sb_s(sb, &t[i]);
}

static void sb_i(sb_t *sb, int32_t v) {
	if (v < 0) { sb_s(sb, "-"); sb_u(sb, (uint32_t)(-v), 0); }
	else sb_u(sb, (uint32_t)v, 0);
}

// -- uptime in seconds, across the tick counter's wrap --

static uint32_t up_last_ticks, up_frac, up_secs;

static uint32_t uptime(void) {
	uint32_t now = z_uptime_ticks();
	uint32_t d = now - up_last_ticks;
	up_last_ticks = now;
	up_frac += d;
	up_secs += up_frac / Z_TICK_HZ;
	up_frac %= Z_TICK_HZ;
	return up_secs;
}

// -- the log --

static void log_line(const char *what, const job_t *j) {

	char line[LINE_MAX + 64];
	sb_t sb = { line, 0, (int)sizeof(line) };

	if (clock_valid()) {
		z_tm_t tm;
		z_time_to_tm(z_tz_local(&tz, clock_now(), NULL), &tm);
		sb_i(&sb, tm.year); sb_s(&sb, "-");
		sb_u(&sb, tm.month, 2); sb_s(&sb, "-");
		sb_u(&sb, tm.day, 2); sb_s(&sb, " ");
		sb_u(&sb, tm.hour, 2); sb_s(&sb, ":");
		sb_u(&sb, tm.min, 2);
	} else {
		sb_s(&sb, "boot+");
		sb_u(&sb, uptime(), 0);
		sb_s(&sb, "s");
	}
	sb_s(&sb, "  ");
	sb_s(&sb, what);
	if (j) {
		sb_s(&sb, " ");
		sb_s(&sb, j->prog);
		if (j->args[0]) { sb_s(&sb, " "); sb_s(&sb, j->args); }
	}
	sb_s(&sb, "\n");

	say("cron: ");
	say(line);

	fs_mkdir("/user");
	int size = fs_size((char *)LOG_PATH);
	int h;
	if (size < 0 || size > LOG_MAX) {
		h = fs_open_write(LOG_PATH);			// new, or cut back to nothing
	} else {
		h = fs_open_rw(LOG_PATH);
		if (h >= 0) fs_seek(h, (uint32_t)size);
	}
	if (h < 0) return;
	fs_write_chunk(h, line, sb.n);
	fs_close_handle(h);

}

// -- the state file: when each job last ran --

static uint32_t hash_line(const char *s, int n) {
	uint32_t h = 2166136261u;
	for (int i = 0; i < n; i++) { h ^= (uint8_t)s[i]; h *= 16777619u; }
	return h ? h : 1;
}

static void state_load(void) {
	int n = fs_read_file((char *)STATE_PATH, cfg_buf, CFG_MAX);
	if (n <= 0) return;
	cfg_buf[n] = 0;
	for (char *p = cfg_buf; *p; ) {
		uint32_t key = 0, t = 0;
		while (*p >= '0' && *p <= '9') key = key * 10 + (uint32_t)(*p++ - '0');
		while (*p == ' ') p++;
		while (*p >= '0' && *p <= '9') t = t * 10 + (uint32_t)(*p++ - '0');
		for (int i = 0; i < njobs; i++) if (jobs[i].key == key) jobs[i].last_run = t;
		while (*p && *p != '\n') p++;
		if (*p) p++;
	}
}

static void state_save(void) {
	char out[MAX_JOBS * 24];
	sb_t sb = { out, 0, (int)sizeof(out) };
	for (int i = 0; i < njobs; i++) {
		if (!jobs[i].last_run) continue;
		sb_u(&sb, jobs[i].key, 0); sb_s(&sb, " ");
		sb_u(&sb, jobs[i].last_run, 0); sb_s(&sb, "\n");
	}
	fs_mkdir("/user");
	fs_write_file((char *)STATE_PATH, out, sb.n);
}

// -- the config --

static bool word_is(const char *a, const char *b) {
	for (; *a && *b; a++, b++) {
		char c = *a;
		if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
		if (c != *b) return false;
	}
	return !*a && !*b;
}

static bool parse_uint(const char *s, uint32_t *v) {
	uint32_t n = 0;
	if (!*s) return false;
	for (; *s; s++) {
		if (*s < '0' || *s > '9') return false;
		n = n * 10 + (uint32_t)(*s - '0');
	}
	*v = n;
	return true;
}

// "HH:MM", or ":MM" when hour_ok is false.
static bool parse_hm(const char *s, bool hour_ok, uint8_t *h, uint8_t *m) {
	const char *c = strchr(s, ':');
	if (!c) return false;
	char hb[4] = {0};
	uint32_t hv = 0, mv;
	if (c - s > 2) return false;
	memcpy(hb, s, (size_t)(c - s));
	if (hour_ok) { if (!parse_uint(hb, &hv) || hv > 23) return false; }
	else if (c != s) return false;
	if (!parse_uint(c + 1, &mv) || mv > 59) return false;
	*h = (uint8_t)hv;
	*m = (uint8_t)mv;
	return true;
}

static int parse_wday(const char *s) {
	static const char *names[7] = { "sun", "mon", "tue", "wed", "thu", "fri", "sat" };
	char w[4] = {0};
	for (int i = 0; i < 3 && s[i]; i++)
		w[i] = (char)((s[i] >= 'A' && s[i] <= 'Z') ? s[i] + 32 : s[i]);
	for (int i = 0; i < 7; i++) if (!strcmp(w, names[i])) return i;
	return -1;
}

static void bad_line(int lineno, const char *why) {
	char m[80];
	sb_t sb = { m, 0, (int)sizeof(m) };
	sb_s(&sb, "cron: " CFG_PATH " line ");
	sb_u(&sb, (uint32_t)lineno, 0);
	sb_s(&sb, ": ");
	sb_s(&sb, why);
	sb_s(&sb, "\n");
	say(m);
}

// One line: a setting, or a rule. Returns false for a line it cannot use.
static void parse_line(char *line, int lineno) {

	char tok[2 * LINE_MAX + 16];
	char *av[24];
	uint8_t fl[24];
	int ac = z_args_split(line, tok, sizeof(tok), av, fl, 24);
	for (int i = 0; i < ac; i++) z_args_unmark(av[i]);
	if (!ac || av[0][0] == '#') return;

	// A setting: "name: value".
	if (!strcmp(av[0], "wait_for_ntp:")) {
		if (ac < 2) { bad_line(lineno, "wait_for_ntp: yes or no"); return; }
		wait_for_ntp = word_is(av[1], "yes") || word_is(av[1], "on") ||
			word_is(av[1], "true") || !strcmp(av[1], "1");
		return;
	}

	if (njobs >= MAX_JOBS) { bad_line(lineno, "too many jobs (32)"); return; }

	job_t j;
	memset(&j, 0, sizeof(j));
	int cmd;

	if (word_is(av[0], "at") && ac >= 2 && word_is(av[1], "boot")) {
		j.kind = J_BOOT; cmd = 2;
	} else if (word_is(av[0], "every") && ac >= 2) {
		char *u = av[1];
		size_t l = strlen(u);
		uint32_t n, mult;
		char unit = l ? u[l - 1] : 0;
		mult = unit == 's' ? 1 : unit == 'm' ? 60 : unit == 'h' ? 3600 : unit == 'd' ? 86400 : 0;
		if (!mult || l < 2) { bad_line(lineno, "every: e.g. 30m, 2h, 1d"); return; }
		u[l - 1] = 0;
		if (!parse_uint(u, &n) || !n) { bad_line(lineno, "every: e.g. 30m, 2h, 1d"); return; }
		j.kind = J_EVERY; j.every = n * mult; cmd = 2;
		if (j.every < 10) j.every = 10;
	} else if (word_is(av[0], "hourly") && ac >= 2) {
		uint8_t h;
		if (!parse_hm(av[1], false, &h, &j.min)) { bad_line(lineno, "hourly :MM"); return; }
		j.kind = J_HOURLY; cmd = 2;
	} else if (word_is(av[0], "daily") && ac >= 2) {
		if (!parse_hm(av[1], true, &j.hour, &j.min)) { bad_line(lineno, "daily HH:MM"); return; }
		j.kind = J_DAILY; cmd = 2;
	} else if (word_is(av[0], "weekly") && ac >= 3) {
		int d = parse_wday(av[1]);
		if (d < 0 || !parse_hm(av[2], true, &j.hour, &j.min)) {
			bad_line(lineno, "weekly mon HH:MM"); return;
		}
		j.kind = J_WEEKLY; j.wday = (uint8_t)d; cmd = 3;
	} else if (word_is(av[0], "monthly") && ac >= 3) {
		uint32_t d;
		if (!parse_uint(av[1], &d) || d < 1 || d > 31 ||
		    !parse_hm(av[2], true, &j.hour, &j.min)) {
			bad_line(lineno, "monthly DAY HH:MM"); return;
		}
		j.kind = J_MONTHLY; j.mday = (uint8_t)d; cmd = 3;
	} else {
		bad_line(lineno, "not a rule -- see docs/cron.md");
		return;
	}

	if (cmd >= ac) { bad_line(lineno, "no program to run"); return; }

	if (strlen(av[cmd]) >= sizeof(j.prog)) { bad_line(lineno, "program name too long"); return; }
	strcpy(j.prog, av[cmd]);
	if (!z_args_join(ac - cmd - 1, av + cmd + 1, j.args, sizeof(j.args))) {
		bad_line(lineno, "arguments too long"); return;
	}

	jobs[njobs++] = j;

}

// Reads the config. Keeps what it knows about jobs whose line has not
// changed -- when they last ran, whether they are running.
static bool load_config(void) {

	// What carries over a reload, per job -- not the whole table, which
	// would be a second 7KB for the sake of five fields.
	static struct { uint32_t key, last_run, pid, next_up; bool boot_done; } old[MAX_JOBS];
	int nold = njobs;
	for (int i = 0; i < njobs; i++) {
		old[i].key = jobs[i].key;
		old[i].last_run = jobs[i].last_run;
		old[i].pid = jobs[i].pid;
		old[i].next_up = jobs[i].next_up;
		old[i].boot_done = jobs[i].boot_done;
	}

	int n = fs_read_file((char *)CFG_PATH, cfg_buf, CFG_MAX);
	if (n < 0) return false;
	cfg_buf[n] = 0;

	njobs = 0;
	wait_for_ntp = true;

	char tzname[Z_CFG_VAL_MAX];
	if (!z_cfg_get("system.rtc.timezone", tzname, sizeof(tzname)) || !z_tz_parse(tzname, &tz))
		z_tz_parse("UTC", &tz);

	int lineno = 0;
	for (char *p = cfg_buf; *p; ) {
		char *e = p;
		while (*e && *e != '\n') e++;
		char save = *e;
		*e = 0;
		lineno++;
		size_t l = (size_t)(e - p);
		if (l && p[l - 1] == '\r') p[--l] = 0;
		if (l < LINE_MAX) {
			char line[LINE_MAX];
			memcpy(line, p, l + 1);
			int before = njobs;
			parse_line(line, lineno);
			if (njobs > before) jobs[njobs - 1].key = hash_line(p, (int)l);
		} else {
			bad_line(lineno, "line too long");
		}
		*e = save;
		p = *e ? e + 1 : e;
	}

	for (int i = 0; i < njobs; i++)
		for (int k = 0; k < nold; k++)
			if (old[k].key == jobs[i].key) {
				jobs[i].last_run = old[k].last_run;
				jobs[i].pid = old[k].pid;
				jobs[i].boot_done = old[k].boot_done;
				jobs[i].next_up = old[k].next_up;
			}

	return true;

}

// -- running --

static void run_job(job_t *j) {

	uint32_t state = Z_PROC_STATE_UNKNOWN;
	if (j->pid && z_proc_status(j->pid, &state, NULL) == Z_OK &&
	    state == Z_PROC_STATE_RUNNING) {
		log_line("skipped, still running:", j);
		return;
	}

	z_launch_arg_set(j->args);
	j->pid = z_proc_run(j->prog);
	log_line(j->pid ? "started" : "could not start", j);

	if (clock_valid()) {
		j->last_run = clock_now();
		state_save();
	}

}

// Jobs that have finished since we last looked: logged with their status.
static void reap(void) {
	for (int i = 0; i < njobs; i++) {
		job_t *j = &jobs[i];
		if (!j->pid) continue;
		uint32_t state;
		int32_t status = 0;
		if (z_proc_status(j->pid, &state, &status) != Z_OK) continue;
		if (state == Z_PROC_STATE_RUNNING) continue;
		char what[40];
		sb_t sb = { what, 0, (int)sizeof(what) };
		if (state == Z_PROC_STATE_EXITED) { sb_s(&sb, "exited "); sb_i(&sb, status); sb_s(&sb, ":"); }
		else sb_s(&sb, "finished:");
		log_line(what, j);
		j->pid = 0;
	}
}

static uint32_t period_of(const job_t *j) {
	switch (j->kind) {
	case J_HOURLY:  return 3600;
	case J_DAILY:   return 86400;
	case J_WEEKLY:  return 7 * 86400;
	case J_MONTHLY: return 31 * 86400;
	default:        return j->every;
	}
}

static bool matches(const job_t *j, const z_tm_t *tm) {
	if (tm->min != j->min) return false;
	switch (j->kind) {
	case J_HOURLY:  return true;
	case J_DAILY:   return tm->hour == j->hour;
	case J_WEEKLY:  return tm->hour == j->hour && tm->wday == j->wday;
	case J_MONTHLY: return tm->hour == j->hour && tm->day == j->mday;
	default:        return false;
	}
}

// The clock-time jobs, once a minute while the clock is valid.
static void check_clock(uint32_t utc, const z_tm_t *tm) {

	for (int i = 0; i < njobs; i++) {

		job_t *j = &jobs[i];
		if (j->kind == J_BOOT || j->kind == J_EVERY) continue;

		uint32_t period = period_of(j);
		// Not twice for one due time: the hour repeated when summer time
		// ends brings 01:30 round again an hour later.
		uint32_t guard = j->kind == J_HOURLY ? 55 * 60 : 90 * 60;
		bool recent = j->last_run && utc - j->last_run < guard;

		if (matches(j, tm) && !recent) { run_job(j); continue; }

		// Catching up: daily and longer, after being off or unsynced.
		if (j->kind != J_HOURLY && j->last_run &&
		    utc - j->last_run > period + 3600)
			run_job(j);

	}

}

// The clock is right, or we are not waiting for it: `at boot` jobs run,
// `every` jobs start counting.
static void cron_begin(void) {

	say("cron: running\n");

	uint32_t now_up = uptime();
	for (int i = 0; i < njobs; i++) {
		job_t *j = &jobs[i];
		if (j->kind == J_EVERY) j->next_up = now_up + j->every;
		if (j->kind == J_BOOT && !j->boot_done) { j->boot_done = true; run_job(j); }
	}

}

// One pass: reload if the config changed, note finished jobs, run what
// is due. Returns how many seconds to sleep before the next pass.
static uint32_t last_minute;

static uint32_t cron_step(void) {

	uint32_t now_up;
	z_msg_t m;
	while (z_msg_read(&m) == Z_OK) {}

	// `cfg reload` re-reads this file too.
	uint32_t g = z_cfg_generation();
	if (g != cfg_gen) {
		cfg_gen = g;
		if (load_config()) {
			say("cron: reloaded\n");
			now_up = uptime();
			for (int i = 0; i < njobs; i++)
				if (jobs[i].kind == J_EVERY && !jobs[i].next_up)
					jobs[i].next_up = now_up + jobs[i].every;
		}
	}

	reap();

	now_up = uptime();
	uint32_t wake = 60;

	for (int i = 0; i < njobs; i++) {
		job_t *j = &jobs[i];
		if (j->kind != J_EVERY) continue;
		if (now_up >= j->next_up) {
			run_job(j);
			j->next_up += j->every;
			if (j->next_up <= now_up) j->next_up = now_up + j->every;
		}
		uint32_t left = j->next_up - now_up;
		if (left < wake) wake = left;
	}

	if (clock_valid()) {
		uint32_t utc = clock_now();
		uint32_t local = z_tz_local(&tz, utc, NULL);
		uint32_t minute = local / 60;
		if (minute != last_minute) {
			last_minute = minute;
			z_tm_t tm;
			z_time_to_tm(local, &tm);
			check_clock(utc, &tm);
		}
		uint32_t to_minute = 60 - local % 60;
		if (to_minute < wake) wake = to_minute;
	}

	if (wake < 1) wake = 1;
	return wake;

}

#ifndef CRON_HOST_TEST
int main(void) {

	char reg[24];
	if (!z_pid_register("cron", reg, sizeof(reg)) || strcmp(reg, "cron0")) {
		say("cron: already running\n");
		return 0;
	}

	uptime();
	cfg_gen = z_cfg_generation();

	if (!load_config()) {
		say("cron: no " CFG_PATH " -- nothing to do\n");
		return 0;
	}
	state_load();

	// Nothing at all until the clock is right, unless told otherwise.
	if (wait_for_ntp && !clock_valid()) {
		say("cron: waiting for the clock (wait_for_ntp: yes)\n");
		while (!clock_valid()) {
			z_msg_t m;
			while (z_msg_read(&m) == Z_OK) {}
			z_proc_wait(Z_TICK_HZ * 5);
		}
	}

	cron_begin();

	for (;;) z_proc_wait(Z_TICK_HZ * cron_step());

}
#endif
