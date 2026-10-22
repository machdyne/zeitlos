/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * backup -- dated backups of your files on a second drive
 * (docs/backup.md).
 *
 *   backup                         back up now
 *   backup -n                      what would be copied; writes nothing
 *   backup -e                      and eject the drive when done
 *   backup status                  the drive and the newest backup
 *   backup list                    the backups, oldest first
 *   backup files [BACKUP] [DIR]    what a backup had
 *   backup history PATH            every copy of one file
 *   backup restore PATH [BACKUP] [-o OUT] [--replace]
 *   backup check [-a]              every index; -a reads every copy
 *   backup pin BACKUP / unpin BACKUP
 *   backup make-room KB
 *   backup eject                   release the drive
 *   backup watch [PATH...]         back up each save there, 10 s after,
 *                                  until stopped (apps.backup.watch)
 *
 * From posix (`backup`, or `run backup`) its output is in that
 * terminal; from the kernel console, on the console. Stop it with
 * `kill backup0` (posix), `kill <pid>`, or from info: it is safe to
 * stop at any moment -- a backup stopped half way has no index, is
 * invisible, and the next one removes what it left.
 *
 * This file binds bk.c, which does the work and is tested on the host
 * (tests/), to fs (zfsc.c: straight to the fs driver, which serves
 * every volume, so /usb's free space and eject are there too), the
 * clock and the terminal.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"          /* Z_TICK_HZ */
#include "../../common/zport.h"
#include "../../common/zfsc.h"
#include "../../common/zfs.h"
#include "../../common/zcfg.h"
#include "../../common/zrtc.h"
#include "../../common/zproc.h"
#include "../../common/zwin.h"          /* z_launch_arg_take() */
#include "../../common/zargs.h"
#include "../../common/zpaths.h"
#include "../posix/posix.h"             /* PX_STDOUT_TAG */

#include "bk.h"
#include "bk_zfs.h"
#include "bkwin.h"
#include "bkwatch.h"

void uart_putc(char c);

// -- settings (docs/backup.md, "Settings") -----------------------------------

#define DEF_TO      Z_DIR_USB "/backup"
#define DEF_FROM    Z_DIR_HOME " " Z_DIR_DATA " " Z_PATH_SYS_CONFIG " /sys/net.cfg"
#define DEF_SKIP    Z_DIR_ASK_PACKS " *.tmp"
#define DEF_NAME    "zeitlos"
#define MAX_LIST    16

static char cfg_to[Z_CFG_VAL_MAX], cfg_from[Z_CFG_VAL_MAX], cfg_skip[Z_CFG_VAL_MAX];
static char cfg_name[Z_CFG_VAL_MAX];
static char from_buf[2 * Z_CFG_VAL_MAX + 2], skip_buf[2 * Z_CFG_VAL_MAX + 2];
static char *from_v[MAX_LIST], *skip_v[MAX_LIST];

static void get(const char *key, const char *def, char *out) {
	if (!z_cfg_get(key, out, Z_CFG_VAL_MAX) || !out[0]) snprintf(out, Z_CFG_VAL_MAX, "%s", def);
}

void backup_settings(bk_opts_t *o) {
	memset(o, 0, sizeof *o);
	get("apps.backup.to", DEF_TO, cfg_to);
	o->quiet_missing = !z_cfg_get("apps.backup.from", cfg_from, sizeof cfg_from) || !cfg_from[0];
	get("apps.backup.from", DEF_FROM, cfg_from);
	get("apps.backup.skip", DEF_SKIP, cfg_skip);
	get("apps.backup.name", DEF_NAME, cfg_name);
	// a trailing slash on the folder would make names with "//"
	size_t n = strlen(cfg_to);
	while (n > 1 && cfg_to[n - 1] == '/') cfg_to[--n] = 0;
	o->dest = cfg_to;
	o->name = cfg_name;
	o->nfrom = z_args_split(cfg_from, from_buf, sizeof from_buf, from_v, NULL, MAX_LIST);
	o->from = (const char *const *)from_v;
	// "none" (or "-") for nothing skipped beyond the fixed rules
	if (!strcmp(cfg_skip, "none") || !strcmp(cfg_skip, "-")) o->nskip = 0;
	else o->nskip = z_args_split(cfg_skip, skip_buf, sizeof skip_buf, skip_v, NULL, MAX_LIST);
	o->skip = (const char *const *)skip_v;
	o->keep = (int)z_cfg_get_int("apps.backup.keep", 10);
	if (o->keep < 0) o->keep = 0;
	o->refresh = 10;
	o->verify = z_cfg_get_bool("apps.backup.verify", true);
	o->manifest = Z_PATH_SYS_MANIFEST;
	o->reserve_kb = 1024;
}

// -- output: posix's terminal, or the console --------------------------------

static z_port_t out;
static bool out_on;

// `backup watch`'s watch: a notice that the wait for an ack below takes
// is handed on to it, not lost.
static bkw_t cli_w;
static bool cli_watching;

static void out_raw(const char *b, int n) {
	if (!out_on) {
		for (int i = 0; i < n; i++) uart_putc(b[i]);
		return;
	}
	// At most 8 sends unacknowledged (zport.h): wait for an ack. fs's
	// replies are taken by tag (zfsc.c), so the mailbox holds only
	// posix's acks and, perhaps, its close.
	uint32_t t0 = z_uptime_ticks();
	while (z_port_send(&out, b, (uint32_t)n) != Z_OK) {
		z_msg_t m;
		if (!out_on || z_port_peer_gone(&out) || z_uptime_ticks() - t0 > Z_TICK_HZ * 5) {
			out_on = false;     // the terminal went: the console, then
			for (int i = 0; i < n; i++) uart_putc(b[i]);
			return;
		}
		if (z_msg_recv(&m, Z_TICK_HZ / 50) == Z_OK) {
			if (m.subject == Z_PORT_DATA_ACK) z_port_handle_ack(&out, &m);
			else if (cli_watching && bkw_msg(&cli_w, &m)) { }
			else if (m.subject == Z_PORT_CLOSE && m.from == out.peer_pid) out_on = false;
		}
	}
}

// A progress line is up (it ends in \r, not \n): the next text clears it.
static int prog_len;

static void put(const char *t) {
	char b[512];
	int n = 0;
	if (prog_len) {
		b[n++] = '\r';
		for (int i = 0; i < prog_len && n < 200; i++) b[n++] = ' ';
		b[n++] = '\r';
		prog_len = 0;
	}
	for (; *t; t++) {
		if (n > (int)sizeof b - 3) { out_raw(b, n); n = 0; }
		if (*t == '\n') b[n++] = '\r';
		b[n++] = *t;
	}
	if (n) out_raw(b, n);
}

static void io_out(void *c, const char *t) { (void)c; put(t); }

static void say(const char *fmt, ...) {
	char b[384];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof b, fmt, ap);
	va_end(ap);
	put(b);
}

// While a large file is copied, its percentage on one line, twice a
// second at most.
static uint32_t prog_t;

static void io_progress(void *c, const bk_progress_t *p) {
	(void)c;
	if (p->phase != BK_PH_COPY || !p->path || p->bytes_total < 256 * 1024) return;
	uint32_t now = z_uptime_ticks();
	if (now - prog_t < Z_TICK_HZ / 2) return;
	prog_t = now;
	char b[160];
	unsigned pct = (unsigned)(p->bytes_done * 100 / p->bytes_total);
	int n = snprintf(b, sizeof b, "  %u%%  %u of %u files  %.80s", pct,
		(unsigned)p->files_done, (unsigned)p->files_total, p->path);
	put("");                // clears the one before
	out_raw("\r", 1);
	out_raw(b, n);
	prog_len = n;
}

// -- files: bk_zfs.c (straight to fs; host-tested in tests/test_fat.c) --

// -- the machine --------------------------------------------------------------

static z_tz_t tz;

static bool io_clock(void *c, bk_tm_t *t, uint32_t *up) {
	(void)c;
	*up = z_uptime_ticks() / Z_TICK_HZ;
	memset(t, 0, sizeof *t);
	if (!z_rtc_available() || !z_rtc_valid()) return false;
	z_tm_t tm;
	z_time_to_tm(z_tz_local(&tz, z_rtc_seconds(), NULL), &tm);
	t->year = (int)tm.year; t->month = tm.month; t->day = tm.day;
	t->hour = tm.hour; t->min = tm.min; t->sec = tm.sec;
	return true;
}

static void io_sleep(void *c, int ms) {
	(void)c;
	uint32_t t = (uint32_t)ms * Z_TICK_HZ / 1000;
	z_proc_wait(t ? t : 1);
}

// LOCK.TXT names its owner: pid, serial (zproc.h: unique to that
// process) and the uptime it was taken at -- after a reboot the uptime
// is smaller, and the lock is stale whoever has that pid now.
static void io_owner(void *c, char *b, int len) {
	(void)c;
	uint32_t pid = z_getpid();
	snprintf(b, (size_t)len, "pid %u serial %u up %u", (unsigned)pid,
		(unsigned)z_proc_serial(pid), (unsigned)(z_uptime_ticks() / Z_TICK_HZ));
}

static bool io_owner_alive(void *c, const char *who) {
	(void)c;
	unsigned pid = 0, serial = 0, up = 0;
	const char *p = who;
	if (strncmp(p, "pid ", 4)) return false;
	pid = (unsigned)strtoul(p + 4, (char **)&p, 10);
	if (strncmp(p, " serial ", 8)) return false;
	serial = (unsigned)strtoul(p + 8, (char **)&p, 10);
	if (strncmp(p, " up ", 4)) return false;
	up = (unsigned)strtoul(p + 4, NULL, 10);
	if (z_uptime_ticks() / Z_TICK_HZ < up) return false;    // rebooted since
	if (pid == z_getpid()) return false;                    // ours: not held
	return serial && z_proc_serial(pid) == serial;
}

static bk_io_t IO;

// The files, the clock and the lock's owner: what the window's jobs use
// too (bkwin.c), which bring their own output and progress.
void backup_base_io(bk_io_t *io) {
	memset(io, 0, sizeof *io);
	bk_zfs_io(io);
	io->clock = io_clock;
	io->sleep_ms = io_sleep;
	io->owner = io_owner;
	io->owner_alive = io_owner_alive;
}

void backup_tz(void) {
	static char tzs[Z_CFG_VAL_MAX];
	z_cfg_get("system.rtc.timezone", tzs, sizeof tzs);
	z_tz_parse(tzs, &tz);
}

static void io_setup(void) {
	backup_base_io(&IO);
	IO.out = io_out;
	IO.progress = io_progress;
}

// -- the commands -------------------------------------------------------------

static void usage(void) {
	put("usage: backup [-n] [-e]                  back up now (-n: what would be done; -e: then eject)\n"
	    "       backup now                        the same (from the console, where no arguments open the window)\n"
	    "       backup window                     the window\n"
	    "       backup status                     the drive and the newest backup\n"
	    "       backup list                       the backups, oldest first\n"
	    "       backup files [BACKUP] [DIR]       what a backup had\n"
	    "       backup history PATH               every copy of one file\n"
	    "       backup restore PATH [BACKUP] [-o OUT] [--replace]\n"
	    "       backup check [-a]                 every index (-a: and every copy)\n"
	    "       backup pin BACKUP | unpin BACKUP\n"
	    "       backup make-room KB\n"
	    "       backup eject                      release the drive\n"
	    "       backup watch [PATH...]            back up each save there, 10 s after, until stopped\n"
	    "BACKUP: a name or its start (2026-10-21); the newest if left out\n"
	    "settings: apps.backup.* in /sys/zeitlos.cfg; see docs/backup.md\n");
}

static void kb_text(uint64_t kb, char *b, size_t n) {
	if (kb < 10 * 1024) snprintf(b, n, "%u KB", (unsigned)kb);
	else if (kb < 10 * 1024 * 1024) snprintf(b, n, "%u MB", (unsigned)(kb / 1024));
	else snprintf(b, n, "%u.%u GB", (unsigned)(kb / (1024 * 1024)),
		(unsigned)(kb % (1024 * 1024) * 10 / (1024 * 1024)));
}

static void bytes_text(uint64_t b, char *o, size_t n) {
	if (b < 10 * 1024) snprintf(o, n, "%u bytes", (unsigned)b);
	else kb_text(b / 1024, o, n);
}

// A name as people read it: "2026-10-21 11:30:48", or "after 2026-10-21
// 11:30:48 (clock not set)".
static void name_text(const char *name, char *o, size_t n) {
	char base[20];
	int k;
	bool w;
	if (!bk_name_parse(name, base, &k, &w)) { snprintf(o, n, "%s", name); return; }
	char t[24];
	snprintf(t, sizeof t, "%.10s %.2s:%.2s:%.2s", base, base + 11, base + 14, base + 17);
	if (!k) snprintf(o, n, "%s%s", t, w ? " (watch)" : "");
	else if (!strncmp(base, "0000", 4)) snprintf(o, n, "#%d, clock not set%s", k, w ? " (watch)" : "");
	else snprintf(o, n, "after %s (clock not set)%s", t, w ? " (watch)" : "");
}

typedef struct { int n, complete, cut; bk_info_t newest, watch; } st_t;

static int status_fn(void *arg, const bk_info_t *b) {
	st_t *s = arg;
	s->n++;
	if (b->complete) { s->complete++; if (b->watch) s->watch = *b; else s->newest = *b; }
	else s->cut++;
	return 0;
}

static int cmd_status(bk_opts_t *o) {
	char vol[16], free_t[24], tot_t[24];
	z_fs_vfs_t v;
	snprintf(vol, sizeof vol, "%s", o->dest);
	char *s = strchr(vol + 1, '/');
	if (s) *s = 0;
	if (zfs_statvfs(vol, &v) < 0 || !v.total_kb) {
		say("drive:   %s is not there%s\n", vol, !strcmp(vol, Z_DIR_USB) ?
			" (not plugged in, or not FAT: exFAT and NTFS cannot be read here)" : "");
		say("backups: in %s/%s, when the drive is there\n", o->dest, o->name);
		return BK_FAILED;
	}
	kb_text(v.free_kb, free_t, sizeof free_t);
	kb_text(v.total_kb, tot_t, sizeof tot_t);
	say("drive:   %s, %s free of %s\n", vol, free_t, tot_t);
	st_t st;
	memset(&st, 0, sizeof st);
	bk_opts_t q = *o;
	bk_list(&IO, &q, status_fn, &st);
	if (!st.complete) say("backups: none yet in %s/%s\n", o->dest, o->name);
	else {
		char t[64];
		name_text(st.newest.name, t, sizeof t);
		say("backups: %d in %s/%s%s\n", st.complete, o->dest, o->name,
			st.cut ? " (and one cut short, removed by the next)" : "");
		if (st.newest.name[0]) say("last:    %s, %u files\n", t, (unsigned)st.newest.files);
		if (st.watch.name[0] && (!st.newest.name[0] || bk_name_cmp(st.watch.name, st.newest.name) > 0)) {
			name_text(st.watch.name, t, sizeof t);
			say("watch:   %s, %u file%s\n", t, (unsigned)st.watch.files, st.watch.files == 1 ? "" : "s");
		}
	}
	{
		char spec[Z_CFG_VAL_MAX];
		if (z_cfg_get("apps.backup.watch", spec, sizeof spec) && spec[0] && strcmp(spec, "none"))
			say("watches: %s (while the window or `backup watch` runs)\n", spec);
	}
	say("from:    %s\n", cfg_from);
	return BK_DONE;
}

static int list_fn(void *arg, const bk_info_t *b) {
	(void)arg;
	char t[64], c[24];
	name_text(b->name, t, sizeof t);
	if (!b->complete) { say("  %-26s cut short (the next backup removes it)\n", b->name); return 0; }
	bytes_text(b->bytes, c, sizeof c);
	say("  %-26s %5u file%s, %u copied (%s)%s  %s\n", b->name, (unsigned)b->files,
		b->files == 1 ? " " : "s", (unsigned)b->copied, c, b->pinned ? ", pinned" : "", t);
	return 0;
}

static int files_fn(void *arg, const bk_file_t *f) {
	(void)arg;
	say("  %10u  %s%s\n", (unsigned)f->size, f->path,
		f->where[0] == '-' ? "  (installed by update)" : f->present ? "" : "  (removed to make room)");
	return 0;
}

static int hist_fn(void *arg, const char *b, const bk_stat_t *st) {
	(void)arg;
	char t[64];
	name_text(b, t, sizeof t);
	say("  %-26s %10u bytes  %s\n", b, (unsigned)st->size, t);
	return 0;
}

static int cmd_eject(const bk_opts_t *o) {
	char vol[16];
	snprintf(vol, sizeof vol, "%s", o->dest);
	char *s = strchr(vol + 1, '/');
	if (s) *s = 0;
	if (strcmp(vol, Z_DIR_USB)) {
		say("backup: the backup folder is on %s; only %s is ejected\n", vol, Z_DIR_USB);
		return BK_USAGE;
	}
	int32_t r = zfs_umount(Z_DIR_USB);
	if (r >= 0) { say("backup: %s released; the drive can be removed\n", Z_DIR_USB); return BK_DONE; }
	if (-r == Z_EBUSY) say("backup: a file on the drive is open; close it first\n");
	else say("backup: %s could not be released\n", Z_DIR_USB);
	return BK_FAILED;
}

// The volume a path is on: "/usb" for "/usb/backup", "/" for the card.
static void vol_of(const char *p, char *vol, size_t n) {
	snprintf(vol, n, "%s", p);
	char *s = strchr(vol + 1, '/');
	if (s) *s = 0;
	if (strcmp(vol, Z_DIR_USB) && strcmp(vol, Z_DIR_RAM) && strcmp(vol, Z_DIR_TMP)) snprintf(vol, n, "/");
}

// How many turns `backup watch` takes; < 0 until it is stopped (the
// host test gives it a few).
static int watch_turns = -1;

static int cmd_watch(bk_opts_t *o, int argc, char **argv) {
	char spec[2 * Z_CFG_VAL_MAX], vol[16];
	spec[0] = 0;
	if (argc > 1) {         // the paths given, quoted again for bkw_start()
		size_t k = 0;
		for (int i = 1; i < argc && k < sizeof spec - 4; i++)
			k += (size_t)snprintf(spec + k, sizeof spec - k, "%s\"%s\"", i > 1 ? " " : "", argv[i]);
	} else if (!z_cfg_get("apps.backup.watch", spec, sizeof spec)) spec[0] = 0;
	if (!bkw_start(&cli_w, o, spec)) {
		if (bkw_why(&cli_w)[0]) say("backup: not watching: %s\n", bkw_why(&cli_w));
		else say("backup: nothing to watch: name folders, `backup watch /home/book`, or set apps.backup.watch\n");
		return BK_USAGE;
	}
	if (bkw_why(&cli_w)[0]) say("backup: not watched: %s\n", bkw_why(&cli_w));
	for (int i = 0; i < cli_w.n; i++) say("backup: watching %s\n", cli_w.what[i]);
	say("backup: each save there is backed up %d s after; stop with Ctrl+C or `kill backup0`\n", BKW_QUIET);
	cli_watching = true;
	vol_of(o->dest, vol, sizeof vol);
	bool said_gone = false;
	int rc = BK_DONE;
	for (int turn = 0; watch_turns < 0 || turn < watch_turns; turn++) {
		z_msg_t m;
		if (z_msg_recv(&m, Z_TICK_HZ) == Z_OK) {
			if (bkw_msg(&cli_w, &m)) { }
			else if (out_on && m.subject == Z_PORT_CLOSE && m.from == out.peer_pid) break;  // its terminal went
			else if (m.subject == Z_PORT_DATA_ACK && out_on) z_port_handle_ack(&out, &m);
		}
		uint32_t now = z_uptime_ticks();
		bkw_poll(&cli_w, now);
		if (!bkw_due(&cli_w, now)) continue;
		z_fs_info_t fi;
		if (zfs_stat(vol, &fi) < 0) {
			if (!said_gone) say("backup: %s is not there: saves are not backed up until it is\n", vol);
			said_gone = true;
			bkw_drop(&cli_w);
			continue;
		}
		said_gone = false;
		const char *only[BKW_PENDING];
		bk_opts_t q = *o;
		bk_result_t r;
		q.only = only;
		q.nonly = bkw_take(&cli_w, only, BKW_PENDING);
		rc = bk_backup(&IO, &q, &r);
		bkw_done(&cli_w);
	}
	cli_watching = false;
	bkw_stop(&cli_w);
	return rc;
}

static int run(int argc, char **argv) {
	bk_opts_t o;
	bk_result_t r;
	backup_settings(&o);
	backup_tz();

	const char *cmd = argc ? argv[0] : "";
	if (!argc || argv[0][0] == '-') {
		bool eject = z_cfg_get_bool("apps.backup.eject", false);
		for (int i = 0; i < argc; i++) {
			if (!strcmp(argv[i], "-n")) o.dry_run = true;
			else if (!strcmp(argv[i], "-e")) eject = true;
			else { usage(); return BK_USAGE; }
		}
		int rc = bk_backup(&IO, &o, &r);
		if (rc <= BK_PARTIAL && eject && !o.dry_run) cmd_eject(&o);
		return rc;
	}
	if (!strcmp(cmd, "-h") || !strcmp(cmd, "--help") || !strcmp(cmd, "help")) { usage(); return BK_DONE; }
	if (!strcmp(cmd, "status")) return cmd_status(&o);
	if (!strcmp(cmd, "list")) {
		say("%s/%s:\n", o.dest, o.name);
		return bk_list(&IO, &o, list_fn, NULL);
	}
	if (!strcmp(cmd, "files"))
		return bk_files(&IO, &o, argc > 1 ? argv[1] : NULL, argc > 2 ? argv[2] : "/", files_fn, NULL);
	if (!strcmp(cmd, "history")) {
		if (argc < 2) { usage(); return BK_USAGE; }
		return bk_history(&IO, &o, argv[1], hist_fn, NULL);
	}
	if (!strcmp(cmd, "restore")) {
		const char *path = NULL, *which = NULL, *to = NULL;
		bool replace = false;
		for (int i = 1; i < argc; i++) {
			if (!strcmp(argv[i], "-o") && i + 1 < argc) to = argv[++i];
			else if (!strcmp(argv[i], "--replace")) replace = true;
			else if (!path) path = argv[i];
			else if (!which) which = argv[i];
			else { usage(); return BK_USAGE; }
		}
		if (!path) { usage(); return BK_USAGE; }
		return bk_restore(&IO, &o, which, path, to, replace, &r);
	}
	if (!strcmp(cmd, "check")) return bk_check(&IO, &o, argc > 1 && !strcmp(argv[1], "-a"), &r);
	if (!strcmp(cmd, "pin") || !strcmp(cmd, "unpin")) {
		if (argc < 2) { usage(); return BK_USAGE; }
		return bk_pin(&IO, &o, argv[1], cmd[0] == 'p');
	}
	if (!strcmp(cmd, "make-room")) {
		if (argc < 2 || atoi(argv[1]) <= 0) { usage(); return BK_USAGE; }
		return bk_make_room(&IO, &o, (uint32_t)atoi(argv[1]), &r);
	}
	if (!strcmp(cmd, "eject")) return cmd_eject(&o);
	if (!strcmp(cmd, "watch")) return cmd_watch(&o, argc, argv);
	say("backup: no command %s\n", cmd);
	usage();
	return BK_USAGE;
}

int main(void) {
	static char line[Z_WM_ARG_MAX + 1], buf[2 * Z_WM_ARG_MAX + 2];
	char *argv[16];
	int argc = 0;

	if (z_launch_arg_take(line, sizeof line) && line[0])
		argc = z_args_split(line, buf, sizeof buf, argv, NULL, 16);

	uint32_t pid = 0;
	if (z_pid_lookup("posix0", &pid) && pid &&
			z_port_connect_arg(&out, pid, z_obj_str(PX_STDOUT_TAG)) == Z_OK)
		out_on = true;

	// A name for `kill backup0`. A second backup (a `list` while one
	// runs) gets backup1; bk.c's lock keeps two from writing at once.
	char me[24];
	z_pid_register("backup", me, sizeof me);

	// The window: from the dock or files (no arguments, and no terminal
	// started it), or `backup window` from a shell. `run backup` at the
	// kernel console with wm running is the first; `run backup now`
	// backs up there.
	bool window = (argc == 0 && !out_on) || (argc == 1 && !strcmp(argv[0], "window"));
	if (window) {
		if (out_on) { z_port_drain(&out, Z_TICK_HZ * 2); z_port_close(&out); out_on = false; }
		backup_tz();
		int wr = bkwin_run();
		if (wr != BKWIN_NO_WM) return wr;
		argc = 0;           // no wm: back up on the console, as before
	}
	if (argc == 1 && !strcmp(argv[0], "now")) argc = 0;

	io_setup();
	int rc = run(argc, argv);

	if (out_on) {
		z_port_drain(&out, Z_TICK_HZ * 2);
		z_port_close(&out);
	}
	return rc;
}
