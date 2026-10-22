/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * backup's command line on the host (docs/backup.md, "Testing"): the
 * app itself, backup.c, run command by command -- settings from a
 * zeitlos.cfg, the arguments as its launch argument, the output as the
 * console gets it -- over the real fs driver and FatFs (test_fs.c's
 * kernel stubs, as test_fat.c), with the stubs below for the rest of
 * the kernel it touches. The clock is not set (backup.c reads the RTC
 * through register accesses the host has not got: io_clock is replaced
 * by one that says so).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define USB_SECTORS (40u * 2048u)
#define main test_fs_main
#include "../../fs/tests/test_fs.c"
#undef main
#undef CHECK

#include "zcfg.h"
#include "zport.h"
#include "zwin.h"

static int tchecks, tfails;
#define T(c, ...) do { tchecks++; if (!(c)) { tfails++; \
	printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// -- the rest of the kernel, as far as backup.c needs it --------------------

static struct { char k[64], v[128]; } cfgs[16];
static int ncfg;
static void cfg_set(const char *k, const char *v) {
	for (int i = 0; i < ncfg; i++) if (!strcmp(cfgs[i].k, k)) { snprintf(cfgs[i].v, 128, "%s", v); return; }
	snprintf(cfgs[ncfg].k, 64, "%s", k); snprintf(cfgs[ncfg].v, 128, "%s", v); ncfg++;
}
bool z_cfg_get(const char *key, char *out, size_t n) {
	for (int i = 0; i < ncfg; i++) if (!strcmp(cfgs[i].k, key)) { snprintf(out, n, "%s", cfgs[i].v); return true; }
	if (n) out[0] = 0;
	return false;
}
bool z_cfg_get_bool(const char *key, bool def) {
	char v[128];
	if (!z_cfg_get(key, v, sizeof v)) return def;
	return !strcmp(v, "yes") || !strcmp(v, "true") || !strcmp(v, "on") || !strcmp(v, "1");
}
int32_t z_cfg_get_int(const char *key, int32_t def) {
	char v[128];
	return z_cfg_get(key, v, sizeof v) ? (int32_t)atoi(v) : def;
}

static char launch[Z_WM_ARG_MAX + 1];
bool z_launch_arg_take(char *out, int n) { snprintf(out, (size_t)n, "%s", launch); return true; }
bool z_pid_register(const char *b, char *out, uint32_t n) { snprintf(out, n, "%s0", b); return true; }
uint32_t z_getpid(void) { return client; }

static char said[65536];
static size_t nsaid;
void uart_putc(char c) { if (nsaid < sizeof said - 1) { said[nsaid++] = c; said[nsaid] = 0; } }

// no posix: the output goes to the console (uart_putc)
z_obj_t z_obj_str(const char *s) { (void)s; z_obj_t o; memset(&o, 0, sizeof o); return o; }
z_rv z_port_connect_arg(z_port_t *p, uint32_t pid, z_obj_t a) { (void)p; (void)pid; (void)a; return Z_FAIL; }
z_rv z_port_send(z_port_t *p, const void *d, uint32_t n) { (void)p; (void)d; (void)n; return Z_FAIL; }
bool z_port_peer_gone(const z_port_t *p) { (void)p; return true; }
void z_port_handle_ack(z_port_t *p, const z_msg_t *m) { (void)p; (void)m; }
z_rv z_port_drain(z_port_t *p, uint32_t t) { (void)p; (void)t; return Z_OK; }
void z_port_close(z_port_t *p) { (void)p; }
// `backup watch` waits here: time passes, and the test's hook may save.
static void (*recv_hook)(int turn);
static int recv_turn;
static uint32_t notices_given;
z_rv z_msg_recv(z_msg_t *m, uint32_t t) {
	if (notices_given < changed_n) {        // what fs sent the watcher
		notices_given = changed_n;
		memset(m, 0, sizeof *m);
		m->subject = Z_FS_CHANGED;
		m->tag = changed_tag;
		m->from = FS_PID;
		return Z_OK;
	}
	ticks += t;
	if (recv_hook) recv_hook(recv_turn++);
	return Z_FAIL;
}

#include "bkwin.h"
// The window is bkwin.c's, tested in tests/render.c: here there is no wm.
int bkwin_run(void) { return BKWIN_NO_WM; }

#define main backup_main
#include "../backup.c"
#undef main

static bool no_clock(void *c, bk_tm_t *t, uint32_t *up) {
	(void)c; memset(t, 0, sizeof *t); *up = z_uptime_ticks() / Z_TICK_HZ; return false;
}

static bool show;

// One command, as typed: its exit status; what it said is in `said`.
static int cmd(const char *line) {
	snprintf(launch, sizeof launch, "%s", line);
	nsaid = 0; said[0] = 0;
	// backup.c's main, with the clock replaced once io_setup() has run
	static char lb[Z_WM_ARG_MAX + 1], buf[2 * Z_WM_ARG_MAX + 2];
	char *argv[16];
	int argc = 0;
	if (z_launch_arg_take(lb, sizeof lb) && lb[0]) argc = z_args_split(lb, buf, sizeof buf, argv, NULL, 16);
	io_setup();
	IO.clock = no_clock;
	ticks += Z_TICK_HZ * 61;    // a minute apart, so +N names count on
	int rc = run(argc, argv);
	if (show) printf("$ backup %s   [exit %d]\n%s", line, rc, said);
	return rc;
}

static bool said_has(const char *s) { return strstr(said, s) != NULL; }

static void card_put(const char *p, const char *text);
// another program saves the book in the third second of `backup watch`
static void save_book(int turn) {
	if (turn != 2) return;
	uint32_t was = client;
	client = 6;
	card_put("/home/book.txt", "Chapter 1\nChapter 2\nChapter 3\n");
	client = was;
}

static void card_put(const char *p, const char *text) {
	int32_t fd = zfs_open(p, Z_O_WRONLY | Z_O_CREAT | Z_O_TRUNC);
	if (fd >= 0) { zfs_write(fd, text, (uint32_t)strlen(text)); zfs_close(fd); }
}

int main(int argc, char **argv) {
	bool verbose = argc > 1 && !strcmp(argv[1], "-v");
	show = verbose;
	char scratch[512], c[1200];
	const char *tmp = getenv("TMPDIR");
	snprintf(scratch, sizeof scratch, "%s/test_cli.XXXXXX", tmp ? tmp : "/tmp");
	if (!mkdtemp(scratch)) return 2;
	card = calloc(CARD_SECTORS, 512);
	ram = calloc(RAM_SECTORS, 512);
	for (int i = 0; i < 64; i++) alive[i] = true;
	fs_test_start(true);
	zfs_mkfs("/");

	card_put("/home/book.txt", "Chapter 1\n");
	zfs_mkdir("/home/My Pics");
	card_put("/home/My Pics/cat one.bmp", "meow meow");
	card_put("/home/draft.tmp", "skip");
	card_put("/sys/zeitlos.cfg", "system.rtc.timezone: Berlin\n");

	// no drive
	T(cmd("") == BK_FAILED && said_has("is the backup drive plugged in?"), "no drive:\n%s", said);
	T(cmd("status") == BK_FAILED && said_has("not plugged in"), "status, no drive:\n%s", said);

	// a FAT32 stick from a shop
	snprintf(c, sizeof c, "mkfs.fat -F 32 -s 1 -C '%s/u.img' %u >/dev/null", scratch, USB_SECTORS / 2);
	if (system(c)) { printf("test_cli: mkfs.fat failed\n"); return 2; }
	snprintf(c, sizeof c, "%s/u.img", scratch);
	FILE *f = fopen(c, "rb");
	usbd = calloc(USB_SECTORS, 512);
	if (!f || fread(usbd, 512, USB_SECTORS, f) != USB_SECTORS) return 2;
	fclose(f);
	usb_present = usb_ready = true; usb_gen++;
	fs_test_usb_poll();

	T(cmd("status") == BK_DONE && said_has("backups: none yet in /usb/backup/zeitlos"), "status, empty:\n%s", said);
	T(cmd("-n") == BK_DONE && said_has("-n: nothing written") && said_has("/home/book.txt (new)"), "-n:\n%s", said);
	T(zfs_stat("/usb/backup", &(z_fs_info_t){0}) < 0, "-n wrote nothing");
	T(cmd("") == BK_DONE && said_has("3 copied"), "backup:\n%s", said);
	T(said_has("\r\n"), "console lines end in CR LF");
	T(said_has("0000-00-00_00-00-00+0001"), "no clock, empty drive: +0001");
	T(!said_has("draft.tmp"), "*.tmp skipped by default");
	T(!said_has("net.cfg"), "a default source that is not there is not mentioned");
	T(cmd("") == BK_DONE && said_has("nothing changed"), "again:\n%s", said);
	card_put("/home/book.txt", "Chapter 1\nChapter 2\n");
	T(cmd("") == BK_DONE && said_has("+0002"), "second:\n%s", said);
	T(cmd("status") == BK_DONE && said_has("backups: 2 in /usb/backup/zeitlos") &&
		said_has("#2, clock not set"), "status:\n%s", said);
	T(cmd("list") == BK_DONE && said_has("+0001") && said_has("+0002"), "list:\n%s", said);
	T(cmd("history /home/book.txt") == BK_DONE && said_has("+0001") && said_has("+0002"), "history:\n%s", said);
	T(cmd("files") == BK_DONE && said_has("/home/My Pics/cat one.bmp"), "files:\n%s", said);
	T(cmd("restore \"/home/My Pics/cat one.bmp\" -o /home/back.bmp") == BK_DONE, "restore a quoted path:\n%s", said);
	z_fs_info_t fi;
	T(zfs_stat("/home/back.bmp", &fi) == 0 && fi.size == 9, "restored");
	T(cmd("restore /home/book.txt 0000-00-00_00-00-00+0001") == BK_DONE &&
		said_has("/home/book (0000-00-00 00-00+1).txt"), "restore beside:\n%s", said);
	T(cmd("check -a") == BK_DONE && said_has("0 problems"), "check -a:\n%s", said);
	T(cmd("pin 0000-00-00_00-00-00+0001") == BK_DONE && said_has("pinned"), "pin:\n%s", said);
	T(cmd("list") == BK_DONE && said_has(", pinned"), "list shows the pin:\n%s", said);
	T(cmd("unpin 0000-00-00_00-00-00+0001") == BK_DONE, "unpin");
	T(cmd("restore") == BK_USAGE && said_has("usage:"), "usage");
	T(cmd("frobnicate") == BK_USAGE && said_has("no command frobnicate"), "unknown command");
	// watch: nothing set, something not backed up, then a save
	T(cmd("watch") == BK_USAGE && said_has("nothing to watch"), "watch, unset:\n%s", said);
	T(cmd("watch /etc") == BK_USAGE && said_has("/etc is not in From"), "watch /etc:\n%s", said);
	watch_turns = 20;
	recv_hook = save_book; recv_turn = 0;
	T(cmd("watch \"/home\" /etc") == BK_DONE, "watch:\n%s", said);
	recv_hook = NULL;
	T(said_has("watching /home") && said_has("not watched: /etc"), "what it watches:\n%s", said);
	T(said_has("1 file to copy") && said_has("/home/book.txt") && said_has("w, "), "a watch backup of the book:\n%s", said);
	T(cmd("list") == BK_DONE && said_has("(watch)"), "list shows it:\n%s", said);
	cfg_set("apps.backup.watch", "/home");
	recv_hook = save_book; recv_turn = 0;
	T(cmd("watch") == BK_DONE && said_has("watching /home") && said_has("nothing written"),
		"the setting; the same text again writes nothing:\n%s", said);
	recv_hook = NULL;
	cfg_set("apps.backup.watch", "");

	// settings: another folder, nothing skipped, a quoted source
	cfg_set("apps.backup.to", "/usb/elsewhere/");
	cfg_set("apps.backup.skip", "none");
	cfg_set("apps.backup.from", "\"/home/My Pics\" /home/draft.tmp /home/nope");
	cfg_set("apps.backup.name", "laptop");
	T(cmd("") == BK_DONE && said_has("/usb/elsewhere/laptop/") && !said_has("//"), "other settings:\n%s", said);
	T(said_has("2 copied"), "the quoted folder and the .tmp file:\n%s", said);
	T(said_has("/home/nope is not there"), "a source of your own that is missing is mentioned");

	// eject: then it is gone, and a backup says so
	T(cmd("eject") == BK_DONE && said_has("released"), "eject:\n%s", said);
	T(zfs_stat("/usb", &fi) < 0, "/usb gone");
	T(cmd("") == BK_FAILED && said_has("plugged in"), "after eject:\n%s", said);
	cfg_set("apps.backup.to", "/home/bk");
	T(cmd("eject") == BK_USAGE && said_has("only /usb"), "eject elsewhere:\n%s", said);

	(void)verbose;
	snprintf(c, sizeof c, "rm -rf '%s'", scratch);
	if (system(c)) {}
	printf("test_cli: %d checks, %d failed\n", tchecks, tfails);
	return tfails != 0;
}
