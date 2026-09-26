/*
 * Zeitlos OS
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * This is the Zeitlos kernel shell / interactive bootloader.
 *
 */

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "../common/zeitlos.h"
#include "../common/zwm.h"	// Z_WM_SET_ARG, for `run`
#include "../common/zargs.h"	// get_arg(): quoting, docs/posix.md
#include "../common/zfs.h"		// Z_FS_PATH_MAX
#include "../common/znet.h"
#include "../common/zstream.h"
#include "../common/zdns.h"
#include "kernel.h"
#include "flashapi.h"
#include "kvstore.h"	// `kv`, and flashtest keeping clear of the store
#include "auth.h"		// `passwd`, `lock`, and the console lock
extern bool k_readline_from_uart;	// kruntime.c
#include "usb/usbh.h"
#include "usb/usbh_cdc.h"
#include "uart.h"
#include "mem.h"
#include "uart.h"
#include "zar.h"
#include "../common/zsoc.h"	// Z_TICK_HZ	// k_uart_getc()/k_uart_rx_empty() -- see
					// boot_cancel_requested() below
#include "../common/zgpio.h"	// the `gpio` command below
#include "fs/fs.h"
#include "fs/sdbench.h"	// sh_sdbench() -- the `sdbench` command, docs/sdcard.md
#include "fsapi.h"
#include "fs/fatfs/ff.h"
#include "fs/fatfs/diskio.h"	// disk_status() -- instrumentation, see below
#include "msg.h"
#include "hid.h"		// k_hid_stats(), HID_FIFO_SIZE -- the `ic` command
#include "pidreg.h"
#include "cfg.h"		// /zeitlos.cfg -- k_cfg_load() at boot, `cfg`
#include "xmodem.h"

// --

char *get_arg(char *str, int n);
/*
 * Parses an unsigned integer. Replaces three sscanf() calls.
 *
 * Those three -- two "%lx" and one "%ld" -- were costing this kernel
 * something like forty kilobytes of FLASH IMAGE, which is a sixth of
 * the 256KB the BIOS copies (docs/kernel.md, "The 256KB image
 * budget"). Not the scanf machinery alone: a generic scanf must be
 * able to handle "%f", so it drags in strtod, strtod drags in the
 * locale tables (`categories`, 13,788 bytes of .rodata) and the
 * decimal power tables the float conversion needs.
 *
 * None of that is reachable from a kernel that only ever parses an
 * address and a pid, and none of it was visible until `make -C sw/os
 * kernel-size` was there to be run. The same trap docs/app_runtime.md
 * documents for apps, in the one place nobody had applied it.
 *
 * Returns 1 on success, 0 on a NULL/empty/malformed argument, which is
 * the same contract the sscanf() calls were being used for -- trailing
 * junk is rejected rather than ignored, which is stricter than sscanf
 * was and better for a command line.
 */
static int parse_uint(const char *s, int base, uint32_t *out) {

	uint32_t v = 0;
	int digits = 0;

	if (!s || !*s) return 0;

	while (*s == ' ' || *s == '\t') s++;
	if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;

	for (; *s; s++) {
		int d;
		if (*s >= '0' && *s <= '9') d = *s - '0';
		else if (base == 16 && *s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
		else if (base == 16 && *s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
		else return 0;
		if (d >= base) return 0;
		v = v * (uint32_t)base + (uint32_t)d;
		digits++;
	}

	if (!digits) return 0;
	*out = v;
	return 1;
}

void sh_help(void);
static void sh_flash_info(void);
static void sh_flashtest(void);
static void sh_bench(void);
void hex_dump(uint32_t addr);
uint32_t xfer_recv(uint32_t addr_ptr);
void cls(void);
void init(void);
void screenshot(void);

// Strict small-decimal parse, for the `gpio` command's port and pin
// arguments.
//
// Strict rather than atoi(), because atoi("out") is 0 -- so
// `gpio 0 out` would silently be read as port 0, pin 0, and drive a
// pin the user never named. Rejecting trailing junk is what turns
// that into an error message.
static bool sh_small_num(const char *s, uint32_t *out) {

	uint32_t v = 0;

	if (!s || !*s) return false;

	for (; *s; s++) {
		if (*s < '0' || *s > '9') return false;
		v = v * 10 + (uint32_t)(*s - '0');
		if (v > 999) return false;		// nothing here is ever that big
	}

	*out = v;

	return true;

}

// framebuffer: fixed native resolution, 1 bit per pixel, packed rows
// -- see docs/gpu_blitter.md/docs/gpu_raster.md for the hardware side
// of this. Shared here by cls() and screenshot() so both stay correct
// together if the resolution ever changes again.
#define FB_BASE   0x20000000
#define FB_WIDTH  640
#define FB_HEIGHT 480
#define FB_SIZE   ((FB_WIDTH * FB_HEIGHT) / 8)   // 38400 bytes

// shortened for now (was 60s) while TFTP is still being brought up --
// waiting a full minute per failed attempt makes debugging painfully
// slow. 10s is still generous for a local network exchange; raise it
// back once TFTP is confirmed working, since a real large-file
// transfer could plausibly need longer.
#define TFTP_REPLY_TIMEOUT_TICKS (10 * 732)

// fs_mount() above is a lazy/deferred mount (FatFs opt=0) -- it
// doesn't actually touch the card at all, the real handshake
// (disk_initialize(), sdmm.c) only happens on the first genuine file
// access. Probing for "wm" immediately after mounting can still race
// that handshake on some hardware (slow card power-up, etc), so
// auto-init polls for it to actually become readable instead of
// assuming a fixed delay is always long enough -- boots as fast as
// the card allows, and still gives a slow card real headroom before
// giving up. Checked once per tick (not in a tight sub-tick loop) so
// this doesn't hammer the card with repeated f_open() attempts.
#define AUTOINIT_TIMEOUT_TICKS (3 * 732)   // ~3 seconds

// Staging buffer for a serial upload (`xf`, `xmf`). The whole file is
// received into RAM and only then written out, because neither
// protocol knows the length up front and both can still fail partway
// through -- staging keeps a failed transfer from leaving a truncated
// file on the card. Allocated per transfer and freed straight after,
// so it costs nothing when no upload is in progress.
#define UPLOAD_MAX_SIZE (1024 * 256)   // 256K max file size for now

// -- core app source selection --
//
// Boot-time only, and deliberately narrow: `ls` doesn't show flash
// apps, `run` doesn't look for them, nothing else consults the
// archive. The whole rule is:
//
//   on the SD card?  -> use that, it is assumed newer
//   otherwise        -> use the flash copy
//
// "Assumed newer" is not a guess dressed up as a policy -- the only
// way an app gets onto the card is somebody deliberately putting it
// there with `xf`, so treating that as intent is exactly right, and it
// keeps single-app hot-swapping working during development with no
// version scheme, timestamps or precedence rules to maintain.
//
// Each app prints where it came from, so a stale file on the card
// shadowing a freshly flashed one is visible at boot rather than a
// mystery later.
typedef enum { CORE_SRC_NONE, CORE_SRC_SD, CORE_SRC_FLASH } core_src_t;

static core_src_t core_exec_info(const char *name, z_exec_info_t *info) {
	if (fs_exec_info_any((char *)name, info) != 0) return CORE_SRC_NONE;
	return fs_exec_is_flash((char *)name) ? CORE_SRC_FLASH : CORE_SRC_SD;
}

static int core_load_exec(uint32_t dst, const char *name,
	const z_exec_info_t *info, core_src_t src) {
	(void)src;	// the resolver re-checks; see fs_load_exec_any()
	return fs_load_exec_any(dst, (char *)name, info);
}

static const char *core_src_name(core_src_t src) {
	return (src == CORE_SRC_FLASH) ? "flash" : "sd";
}

static bool wait_for_apps_ready(void) {
	uint32_t start = z_uptime_ticks();
	uint32_t last_tick = start;
	while (z_uptime_ticks() - start < AUTOINIT_TIMEOUT_TICKS) {
		uint32_t now = z_uptime_ticks();
		if (now == last_tick) continue;
		last_tick = now;
		if (fs_size("wm") != 0) return true;
	}
	return false;
}

// Bring the sdcard up, retrying while a slow one powers on.
//
// This has to happen at boot whether or not the core apps are coming
// from the card, and that is the whole point of it existing
// separately. fs_mount() is a DEFERRED mount (FatFs opt=0): it
// registers the volume without touching the hardware, and the card is
// only really initialised by the first file operation that needs it.
//
// wait_for_apps_ready() above used to do that initialisation by
// accident -- its repeated fs_size("wm") calls were what drove the
// card through disk_initialize(). Once boot could skip that loop (core
// apps in flash, nothing needed from the card), the card was left
// uninitialised and every later operation failed with FR_NOT_READY:
// `ls` showed no files, `xf` refused to write. The card was fine;
// nothing had ever woken it.
//
// So: force the mount, explicitly, always.
static bool wait_for_card_ready(void) {
	uint32_t start = z_uptime_ticks();
	uint32_t last_tick = start;

	if (fs_mount_now() == 0) return true;

	while (z_uptime_ticks() - start < AUTOINIT_TIMEOUT_TICKS) {
		uint32_t now = z_uptime_ticks();
		if (now == last_tick) continue;
		last_tick = now;
		if (fs_mount_now() == 0) return true;
	}

	return false;
}

// resolved once, cached for the shell's lifetime (which is the whole
// uptime of the system, sh.c being pid 0) -- same reasoning as
// zwin.c's resolve_wm_pid(): re-doing a name lookup on every single
// tget/tput would be wasteful when net's pid doesn't change once
// it's running. Falls back to the fixed Z_PID_NET constant (znet.h)
// if lookup ever fails (net hasn't registered yet, or hasn't been
// started at all -- zstream_open()/z_msg_new_send() below still fail
// safely against a wrong/dead pid either way, same as always).
static uint32_t net_pid_cache;
static bool net_pid_resolved = false;

// Returns 0 if net isn't running -- see zdns.c's copy of this for why
// there is no longer a fallback to the fixed Z_PID_NET constant.
//
// Not cached on failure: net may simply not have started yet, and a
// cached 0 would keep reporting that after it had.
static uint32_t resolve_net_pid(void) {
	if (!net_pid_resolved) {
		if (z_pid_lookup("net0", &net_pid_cache))
			net_pid_resolved = true;
		else
			return 0;
	}
	return net_pid_cache;
}

// a fresh tag per tget/tput call, not a constant 0 -- if a request
// times out on the shell side (above) but net's reply arrives later
// anyway, and the user then issues a NEW tget/tput before that stale
// reply shows up, z_msg_wait_timeout() matching on (subject, tag)
// alone could match the stale reply to the new, unrelated request. a
// monotonically increasing tag makes every request distinguishable
// from every other one, so this can't happen.
static uint32_t next_tftp_tag(void) {
	static uint32_t tag = 0;
	return ++tag;
}

// --



// How long to wait for the user to cancel the init script, and which
// key does it. ~500ms at the KTIMER's ~732Hz -- long enough to catch a
// deliberate keypress (and a held key repeats, so it's forgiving),
// short enough that nobody notices it on a normal boot.
#define BOOT_CANCEL_TICKS  ((Z_TICK_HZ * 500) / 1000)
#define BOOT_CANCEL_KEY    0x1b   // ESC

// Gives the user a brief window to stop the graphical environment from
// starting, the same way sw/bios/bios.c's own AUTOLOAD_CNT loop lets a
// keypress stop the BIOS from autoloading the kernel. Returns true if
// init() should be skipped.
//
// Why this exists: once wm starts it clears the screen and takes over,
// and if something in the graphical stack is broken (a bad wm build, an
// app that wedges, a display that shows nothing) there was no way back
// to the serial console short of reflashing. This is the escape hatch.
// The shell prompt is still there afterwards, so `init` can be run by
// hand once whatever it was is sorted out.
//
// SERIAL CONSOLE ONLY, deliberately. k_uart_getc()/k_uart_rx_empty()
// (sw/os/uart.h) read the UART and nothing else -- the USB keyboard
// goes through an entirely separate path (z_hid_read_key(), sw/os/hid.c)
// which is not polled here. That's the right split: this is a recovery
// mechanism for when the graphical side is what's broken, so it should
// depend on as little of the system as possible, and the console is the
// one interface guaranteed to work when the display isn't. It also
// means a stray keypress on the USB keyboard during boot can't silently
// leave someone at a bare shell wondering where their desktop went.
//
// Side benefit worth knowing: the 500ms this costs every boot is also
// 500ms longer that the flash-backed boot splash (sw/os/logo.h) stays
// on screen before wm's clear_screen() wipes it -- which on a monitor
// that takes a moment to sync is the difference between seeing it and
// not.
static bool boot_cancel_requested(void) {

	printf("starting init in 500ms -- press ESC to cancel ... ");
	fflush(stdout);

	uint32_t start = z_uptime_ticks();
	bool cancel = false;

	while (z_uptime_ticks() - start < BOOT_CANCEL_TICKS) {

		if (k_uart_rx_empty()) continue;

		// Anything OTHER than ESC is deliberately discarded rather than
		// treated as a cancel: line noise, or a stray byte left in the
		// FIFO from whatever the user typed at the BIOS prompt, should
		// not silently skip the desktop. One specific key, so cancelling
		// is always something you meant to do.
		if (k_uart_getc() == BOOT_CANCEL_KEY) {
			cancel = true;
			break;
		}

	}

	printf(cancel ? "cancelled.\n" : "\n");

	return cancel;

}


// "cache" shell command (docs/icache.md, docs/dcache.md).
//
//   cache              geometry, hit rates, traffic counters
//   cache on|off       instruction cache
//   cache flush        flush instruction (and data) cache
//   cache don|doff     data cache (loads)
//   cache wbon|wboff   posted writes (write buffer)
//   cache clear        zero the traffic counters
static void sh_cache_rate(const char *name, uint32_t hits, uint32_t misses) {
	uint32_t total = hits + misses;
	uint32_t h = hits;
	uint32_t t = total;
	uint32_t permille;

	// scale both down together before multiplying by 1000, rather
	// than reaching for 64-bit math: h*1000 overflows a uint32_t past
	// ~4.29M hits, which is a perfectly reachable count between two
	// flushes.
	while (t > 4000000u) { t >>= 4; h >>= 4; }
	permille = t ? (h * 1000u) / t : 0;

	printf("  %s hits:   %ld\n", name, (long)hits);
	printf("  %s misses: %ld\n", name, (long)misses);
	if (total)
		printf("  %s rate:   %ld.%ld%%\n", name,
			(long)(permille / 10), (long)(permille % 10));
	else
		printf("  %s rate:   (none yet)\n", name);
}

// "mpu" shell command (docs/mpu.md).
//
//   mpu                  status, violation count, last fault
//   mpu report|enforce   log violations only / end the app
//   mpu off              no checking at all
//   mpu clear            clear the fault registers and the log limit
bool k_mpu_active(void);
void k_mpu_reset_log(void);

static void sh_mpu(const char *arg) {
	if (!z_mpu_present() || !k_mpu_active()) {
		printf("no memory protection unit in this bitstream\n");
		return;
	}
	uint32_t ctrl = reg_mpu_ctrl;
	if (arg != NULL && !strcmp(arg, "enforce")) {
		reg_mpu_ctrl = Z_MPU_CTRL_ENABLE | Z_MPU_CTRL_ENFORCE | Z_MPU_CTRL_IRQ;
		printf("mpu: enforcing (violations end the app)\n");
	} else if (arg != NULL && !strcmp(arg, "report")) {
		reg_mpu_ctrl = Z_MPU_CTRL_ENABLE | Z_MPU_CTRL_IRQ;
		printf("mpu: report-only (violations are logged)\n");
	} else if (arg != NULL && !strcmp(arg, "off")) {
		reg_mpu_ctrl = 0;
		printf("mpu: off\n");
	} else if (arg != NULL && !strcmp(arg, "clear")) {
		reg_mpu_fault_info = 0;
		reg_mpu_count = 0;
		k_mpu_reset_log();
		printf("mpu: cleared\n");
	} else {
		uint32_t info = reg_mpu_fault_info;
		printf("mpu: %s\n", !(ctrl & Z_MPU_CTRL_ENABLE) ? "off" :
			(ctrl & Z_MPU_CTRL_ENFORCE) ? "enforcing" : "report-only");
		printf("  kernel text ends %08lx, gate %08lx, mask %04lx\n",
			(unsigned long)reg_mpu_ktext, (unsigned long)reg_mpu_gate,
			(unsigned long)reg_mpu_mask);
		printf("  violations pending: %ld\n", (long)reg_mpu_count);
		if (info & Z_MPU_FAULT_VALID)
			printf("  first: kind %ld reason %ld, address %08lx, pc %08lx\n",
				(long)Z_MPU_FAULT_KIND(info), (long)Z_MPU_FAULT_REASON(info),
				(unsigned long)reg_mpu_fault_addr,
				(unsigned long)reg_mpu_fault_pc);
	}
}

static void sh_cache(const char *arg) {
	bool d = z_dcache_present();

	if (!z_icache_present()) {
		printf("no cache in this bitstream\n");
		return;
	}
	if (arg != NULL && !strcmp(arg, "on")) {
		z_icache_enable(true);
		printf("instruction cache enabled\n");
	} else if (arg != NULL && !strcmp(arg, "off")) {
		z_icache_enable(false);
		printf("instruction cache disabled\n");
	} else if (arg != NULL && !strcmp(arg, "flush")) {
		z_icache_flush();
		z_dcache_flush();
		printf("cache flushed\n");
	} else if (arg != NULL && (!strcmp(arg, "don") || !strcmp(arg, "doff") ||
			!strcmp(arg, "wbon") || !strcmp(arg, "wboff") ||
			!strcmp(arg, "clear"))) {
		if (!d) {
			printf("no data cache in this bitstream\n");
		} else if (!strcmp(arg, "clear")) {
			z_cache_stats_clear();
			printf("cache counters cleared\n");
		} else {
			bool en = z_dcache_enabled();
			bool wb = z_dcache_posted();
			if (!strcmp(arg, "don")) en = true;
			if (!strcmp(arg, "doff")) en = false;
			if (!strcmp(arg, "wbon")) wb = true;
			if (!strcmp(arg, "wboff")) wb = false;
			z_dcache_set(en, wb);
			printf("data cache %s, posted writes %s\n",
				en ? "enabled" : "disabled", wb ? "on" : "off");
		}
	} else {
		printf("icache: %ldKB, %ld-word lines%s\n",
			(long)z_icache_kb(), (long)z_icache_line_words(),
			(reg_icache_ctrl & Z_ICACHE_CTRL_ENABLE) ? "" : " (DISABLED)");
		sh_cache_rate("I", reg_icache_hits, reg_icache_misses);
		if (!d) return;
		printf("dcache: %ldKB, %ld-word lines, %ld-entry write buffer%s%s%s\n",
			(long)z_dcache_kb(), (long)z_dcache_line_words(),
			(long)z_dcache_wbuf_depth(),
			z_cache_burst() ? ", burst fills" : "",
			z_dcache_enabled() ? "" : " (DISABLED)",
			z_dcache_posted() ? "" : " (writes not posted)");
		sh_cache_rate("D", reg_dcache_hits, reg_dcache_misses);
		printf("  loads:    %ld\n", (long)reg_cache_loads);
		printf("  stores:   %ld\n", (long)reg_cache_stores);
		printf("  stall:    %ld cycles\n", (long)reg_cache_stall);
		printf("  wb full:  %ld cycles\n", (long)reg_cache_wbfull);
		printf("  i-snoops: %ld\n", (long)reg_cache_isnoops);
	}
}

void sh(void) {

   char buffer[256];
   int cmdlen;
   char *cmdend;
	char *arg;

	// explicit reset, not trusted to .bss's initial value -- sh.c is
	// compiled into the kernel binary, and kernel.bin's own objcopy
	// rule (sw/os/Makefile) has no --pad-to, unlike every app's
	// Makefile (confirmed for wm/term/portdemo) -- so, same as
	// sw/os/pidreg.c's k_pidreg_init(), this can't be left to .bss's
	// initial value on real hardware. sh() runs exactly once, here,
	// before anything could possibly reach resolve_net_pid() (tget/
	// tput, or `init` calling it indirectly), so this is the one
	// place that's actually guaranteed early enough.
	net_pid_resolved = false;
	net_pid_cache = 0;

	printf("type help for help.\n\n");

   printf("mounting fs ... ");
   fflush(stdout);

   if (fs_mount() == 0)
      printf("done.\n");
   else
      printf("failed.\n");

	// auto-start the graphical environment by default -- see
	// wait_for_apps_ready()'s own comment above for why this polls
	// rather than just calling init() immediately after fs_mount().
	//
	// The flash case is checked FIRST and skips the poll entirely.
	// wait_for_apps_ready() exists to tolerate a slow SD card powering
	// up, and spends up to AUTOINIT_TIMEOUT_TICKS doing it -- which is
	// exactly the wrong thing to do on a board with no card at all,
	// where the answer will never change and the user would sit
	// through a 3 second stall on every single boot before the desktop
	// appeared. Booting with no card is a first-class path here, not a
	// fallback: see sw/os/zar.h.
	//
	// Note this only decides WHEN to call init(). init() still picks
	// per app, so a card holding just `wm` still gets its wm from the
	// card and everything else from flash.
	// Bring the card up FIRST, always, before deciding anything else.
	// Where the core apps come from is a separate question -- init()
	// resolves that per app -- but `ls`, `xf` and every other file
	// operation afterwards need the card initialised, and with a
	// deferred mount nothing else will do it.
	//
	// When the core apps are in flash we only try once: a board with no
	// card should not stall for AUTOINIT_TIMEOUT_TICKS on every boot
	// waiting for hardware that isn't there. When they are not, the
	// card is the only source of apps, so it is worth waiting for.
	bool card_ready;
	if (z_zar_present()) {
		// INSTRUMENTATION -- remove once the dock-at-boot question is
		// settled.
		//
		// This branch makes exactly ONE attempt, while fs.c's own
		// fs_mount_now() comment says "a slow card may need a few
		// attempts, so callers should retry rather than treating one
		// failure as final". The retry helper (wait_for_card_ready())
		// is right there but is skipped whenever a flash archive is
		// present, so that a cardless board doesn't stall.
		//
		// disk_status() bit 0 is STA_NOINIT (sdmm.c's `Stat`). If that
		// bit is SET while card_ready is true, f_mount returned OK on a
		// card that was never actually initialised -- which is the
		// whole question.
		int mres = fs_mount_now();
		kprint("init: fs_mount_now = 0x");
		kprint_hex32((uint32_t)mres);
		kprint("  disk_status = 0x");
		kprint_hex32((uint32_t)disk_status(0));
		kprint("\n");
		card_ready = (mres == 0);
	} else
		card_ready = wait_for_card_ready();

	if (card_ready)
		printf("init: sdcard ready\n");
	else if (z_zar_present())
		printf("init: no sdcard, using core apps in flash\n");

	// /zeitlos.cfg, before init() and before the cancel window -- so
	// every app init starts sees the settings from its first
	// instruction, and a cancelled boot still has them for `run`. No
	// card or no file loads nothing and everything uses its default.
	// See docs/config.md.
	{
		uint32_t ignored;
		if (card_ready) k_cfg_load(true, &ignored);
		else {
			// Not final: a freshly powered card can fail this first
			// access and come up moments later, when init() loads the
			// shells from it -- and init() retries then (k_cfg_retry(),
			// issue #7). A board with no card never waits for one.
			printf("cfg: no sdcard yet -- init will try again\n");
			k_cfg_defer();
		}
	}

	if (boot_cancel_requested()) {
		printf("init: cancelled -- run `init` to start the graphical "
			"environment manually\n");
	} else if (card_ready || z_zar_present()) {
		init();
	} else if (wait_for_apps_ready()) {
		init();
	} else {
		printf("init: apps not found on filesystem after %lus, "
			"skipping auto-start (run `init` manually once ready)\n",
			(unsigned long)(AUTOINIT_TIMEOUT_TICKS / 732));
	}

	// The console lock (docs/security.md): after init, so a locked
	// console never holds up the desktop, and before the first prompt.
	k_auth_console_boot();

	while (1) {

		printf("> ");
		fflush(stdout);

		readline(buffer, 255);
		cmdend = strchr(buffer, ' ');

		if (cmdend == NULL)
			cmdlen = 255;
		else
			cmdlen = cmdend - buffer;

		printf("\n");

		// HELP
		if (!strncmp(buffer, "help", cmdlen)) sh_help();

		// FLASH: rtl/spiflash.v's identity and state, docs/spiflash.md
		// (strncmp against cmdlen, like every command here: cmdlen is
		// 255 when there are no arguments, so a length test never
		// matches a bare command)
		else if (!strncmp(buffer, "flash", cmdlen)) {
			sh_flash_info();
		}

		// FLASHTEST: the on-board test of writing the flash
		else if (!strncmp(buffer, "flashtest", cmdlen)) {
			sh_flashtest();
		}

		// REBOOT: rtl/socctl.v's RECONFIG, docs/zboot.md
		else if (!strncmp(buffer, "reboot", cmdlen)) {
			k_boot_to(0);           // returns only on failure, having said why
		}

		// JUMP: the jumploader, docs/zboot.md sec. 5
		else if (!strncmp(buffer, "jump", cmdlen)) {
			if (cmdend && cmdend[1]) {
				k_boot_to((uint32_t)strtoul(cmdend + 1, NULL, 16));
			} else {
				uint32_t t;
				printf("jump: this gateware %s through the jumploader\n",
					z_soc_has_feature2(Z_FEATURE2_JUMP) ? "reloads" : "does not reload");
				int jr = k_jump_read(&t);
				if (jr == 0)
					printf("jump: the jumploader at 0x%06lx points at 0x%06lx%s\n",
						(unsigned long)Z_JUMP_FLASH_OFFSET, (unsigned long)t, t ? "" : " (a reboot)");
				else
					k_jump_explain("jump", jr);
			}
		}

		// HEX DUMP
		else if (!strncmp(buffer, "hd", cmdlen)) {
			arg = get_arg(buffer, 1);
			uint32_t addr;
			if (parse_uint(arg, 16, &addr))
				hex_dump(addr);
		}

		// LIST DIRECTORY
		else if (!strncmp(buffer, "ls", cmdlen)) {
			arg = get_arg(buffer, 1);
			if (arg != NULL)
				fs_list_dir(arg);
			else
				fs_list_dir("/");
		}

		// MAKE DIRECTORY
		if (!strncmp(buffer, "mkdir", cmdlen)) {
			arg = get_arg(buffer, 1);
			if (arg != NULL)
				fs_mkdir(arg);
			else
				printf("error: no file/directory specified\n");
		}

		// TOUCH FILE
		if (!strncmp(buffer, "touch", cmdlen)) {
			arg = get_arg(buffer, 1);
			if (arg != NULL)
				fs_touch(arg);
			else
				printf("error: no file/directory specified\n");
		}

		// REMOVE FILE/DIRECTORY
		if (!strncmp(buffer, "rm", cmdlen)) {
			arg = get_arg(buffer, 1);
			if (arg != NULL)
				fs_unlink(arg);
			else
				printf("error: no file/directory specified\n");
		}

		// RECEIVE TO ADDR VIA XFER
		else if (!strncmp(buffer, "xa", cmdlen)) {
			arg = get_arg(buffer, 1);
			uint32_t addr, bytes;
			if (!parse_uint(arg, 16, &addr)) {
				printf("bad address\n");
				continue;
			}
			printf("xfer addr 0x%lx; ready to receive (press D to cancel) ...\n",
				addr);
			bytes = xfer_recv(addr);
			printf("received %li bytes to 0x%lx.\n", bytes, addr);
		}


		// RECEIVE TO FILE VIA XFER
		else if (!strncmp(buffer, "xf", cmdlen)) {
			arg = get_arg(buffer, 1);
			if (arg == NULL) {
				printf("error: no file specified\n");
				continue;
			}

			// for testing purposes; delete it if it already exists
			fs_unlink(arg);

			uint32_t bytes_received, bytes_written;
			void *tmp = k_mem_alloc(UPLOAD_MAX_SIZE);
			uint32_t addr = (uint32_t)(uintptr_t)tmp;
			printf("uploading to file %s.\n", arg);
			printf("xfer addr 0x%lx; ready to receive (press D to cancel) ...\n",
				addr);
			bytes_received = xfer_recv(addr);
			printf("received %li bytes to 0x%lx.\n", bytes_received, addr);
			if (bytes_received) {
				printf("writing to file %s ... ", arg);
				fflush(stdout);
				bytes_written = fs_write_file(arg, tmp, bytes_received);
				k_mem_free(tmp);
				if (bytes_written == bytes_received)
					printf("done.\n");
				else
					printf("failed.\n");
			}
		}

		// RECEIVE TO FILE VIA XMODEM
		//
		// Same shape as `xf` above, different protocol: this one talks
		// to any ordinary terminal program's built-in send, with no
		// host-side tooling. See sw/os/xmodem.h for when to prefer
		// which -- short version, `xf` for executables (exact length),
		// `xmf` for everything else and for machines where you only
		// have a serial terminal.
		else if (!strncmp(buffer, "xmf", cmdlen)) {

			arg = get_arg(buffer, 1);
			if (arg == NULL) {
				printf("error: no file specified\n");
				continue;
			}

			void *tmp = k_mem_alloc(UPLOAD_MAX_SIZE);
			if (tmp == NULL) {
				printf("error: out of memory\n");
				continue;
			}

			printf("uploading to file %s via xmodem.\n", arg);
			printf("start your terminal's xmodem send now; waiting up to "
				"3 minutes ('C' below is the CRC request) ...\n");
			fflush(stdout);

			xmodem_result_t xres;
			uint32_t bytes_received = xmodem_recv(
				(uint32_t)(uintptr_t)tmp, UPLOAD_MAX_SIZE, &xres);

			if (xres != XMODEM_OK) {
				printf("\nxmodem: %s.\n", xmodem_strerror(xres));
				k_mem_free(tmp);
				continue;
			}

			printf("\nreceived %lu bytes.\n", (unsigned long)bytes_received);
			printf("writing to file %s ... ", arg);
			fflush(stdout);

			// FA_CREATE_ALWAYS truncates, so no fs_unlink() needed
			uint32_t bytes_written =
				fs_write_file(arg, tmp, bytes_received);
			k_mem_free(tmp);

			if (bytes_written == bytes_received)
				printf("done.\n");
			else
				printf("failed.\n");
		}

		// GET FILE VIA TFTP (uses the 'net' app -- see sw/common/znet.h)
		else if (!strncmp(buffer, "tget", cmdlen)) {

			char *ip_str = get_arg(buffer, 1);
			char *remote = get_arg(buffer, 2);
			char *local = get_arg(buffer, 3);

			if (!ip_str || !remote) {
				printf("usage: tget <server-ip-or-hostname> <remote-file> [local-file]\n");
				continue;
			}
			if (!local) local = remote;

			uint32_t ip;
			char err[64];
			if (!z_resolve_host(ip_str, &ip, err, sizeof(err))) {
				printf("tget: %s\n", err);
				continue;
			}

			printf("tget: requesting %s from %s ...\n", remote, ip_str);
			fflush(stdout);

			z_obj_t req = z_obj_map(2);
			z_map_set(&req, "ip", z_obj_uint32(ip));
			z_map_set(&req, "filename", z_obj_str(remote));
			// note: `req` intentionally never freed -- one-shot
			// request, same borrowed-payload reasoning used
			// throughout (see docs/messaging.md)

			zstream_consumer_t cons;
			uint32_t npid = resolve_net_pid();
			if (!npid) {
				printf("net is not running (run `init` or `run net`)\n");
				continue;
			}
			if (!zstream_open(&cons, npid, req, err, sizeof(err))) {
				printf("tget: failed to open: %s\n", err);
				continue;
			}

			FIL f;
			if (!fs_open_write(&f, local)) {
				printf("tget: failed to open %s for writing\n", local);
				zstream_abort(&cons);
				continue;
			}

			uint32_t total = 0;
			bool ok = true;

			while (1) {

				const uint8_t *data;
				uint32_t len;
				zstream_result_t r = zstream_pull(&cons, &data, &len, err, sizeof(err));

				if (r == ZSTREAM_EOF) break;

				if (r == ZSTREAM_ERROR) {
					printf("tget: failed: %s\n", err);
					ok = false;
					break;
				}

				if (fs_write_chunk(&f, data, len) != (int)len) {
					printf("tget: write failed\n");
					zstream_abort(&cons);
					ok = false;
					break;
				}

				total += len;

			}

			fs_close_write(&f);

			if (ok) printf("tget: wrote %ld bytes to %s\n", (long)total, local);

		}

		// PUT FILE VIA TFTP
		else if (!strncmp(buffer, "tput", cmdlen)) {

			char *ip_str = get_arg(buffer, 1);
			char *local = get_arg(buffer, 2);
			char *remote = get_arg(buffer, 3);

			if (!ip_str || !local) {
				printf("usage: tput <server-ip-or-hostname> <local-file> [remote-file]\n");
				continue;
			}
			if (!remote) remote = local;

			uint32_t ip;
			char resolve_err[64];
			if (!z_resolve_host(ip_str, &ip, resolve_err, sizeof(resolve_err))) {
				printf("tput: %s\n", resolve_err);
				continue;
			}

			uint32_t size = fs_size(local);
			if (!size) {
				printf("tput: local file not found/empty\n");
				continue;
			}

			FIL f;
			if (!fs_open_read(&f, local)) {
				printf("tput: failed to open %s for reading\n", local);
				continue;
			}

			uint32_t tag = next_tftp_tag();
			z_obj_t req = z_obj_map(2);
			z_map_set(&req, "ip", z_obj_uint32(ip));
			z_map_set(&req, "filename", z_obj_str(remote));
			uint32_t npid2 = resolve_net_pid();
			if (!npid2) {
				printf("net is not running (run `init` or `run net`)\n");
				continue;
			}
			z_msg_new_send(npid2, Z_NET_TFTP_PUT, tag, req);
			// note: `req` intentionally never freed -- same
			// borrowed-payload reasoning as tget above; one-shot
			// per tput call.

			printf("tput: sending %s (%ld bytes) to %s ...\n", local, (long)size, ip_str);
			fflush(stdout);

			// act as a zstream *producer* now -- net is about to
			// open a stream back to us (pid 0) to pull this file's
			// bytes. we have nothing else to do while this runs, so
			// a simple blocking loop is fine here, same reasoning as
			// z_msg_wait_timeout()'s own use below.
			zstream_producer_t prod;
			bool have_stream = false;
			bool producer_ok = true;
			uint8_t chunk[ZSTREAM_CHUNK_SIZE_DEFAULT];
			uint32_t start = z_uptime_ticks();

			while (z_uptime_ticks() - start < TFTP_REPLY_TIMEOUT_TICKS) {

				z_msg_t msg;
				if (z_msg_read(&msg) != Z_OK) continue;

				if (!have_stream) {
					if (msg.subject != Z_STREAM_OPEN) continue;	// discard anything else while waiting to start
					zstream_accept(&prod, msg.from, msg.tag);
					have_stream = true;
					start = z_uptime_ticks();
					continue;
				}

				if (msg.subject == Z_STREAM_ABORT) {
					producer_ok = false;
					break;
				}

				if (msg.subject != Z_STREAM_PULL) continue;

				if (zstream_producer_handle(&prod, &msg) != ZSTREAM_EVENT_PULL)
					continue;	// stale/retry pull, already handled internally

				int32_t n = fs_read_chunk(&f, chunk, sizeof(chunk));

				if (n < 0) {
					zstream_send_error(&prod, "local read failed");
					producer_ok = false;
					break;
				}

				if (n == 0) {
					zstream_send_eof(&prod);
					break;	// our part is done -- net finishes talking to the server on its own
				}

				zstream_send_chunk(&prod, chunk, (uint32_t)n);
				start = z_uptime_ticks();

			}

			fs_close_read(&f);

			if (!have_stream) {
				printf("tput: no reply from net after 10s -- is it running? (`run net`) "
					"if it's running but this happened anyway, that's worth reporting.\n");
				continue;
			}

			if (!producer_ok) {
				printf("tput: failed sending local data\n");
				continue;
			}

			z_msg_t reply;
			if (z_msg_wait_timeout(&reply, Z_NET_TFTP_PUT_REPLY, tag, TFTP_REPLY_TIMEOUT_TICKS) != Z_OK) {
				printf("tput: no reply from net after 10s -- is it running? (`run net`) "
					"if it's running but this happened anyway, that's worth reporting.\n");
				continue;
			}

			z_obj_t *ok = z_map_find(&reply.obj, "ok");
			if (ok && ok->val.uint32) {
				printf("tput: done\n");
			} else {
				z_obj_t *err = z_map_find(&reply.obj, "error");
				printf("tput: failed: %s\n",
					(err && err->type == Z_STR) ? err->val.str : "unknown error");
			}

		}

		// CREATE A PROCESS
		else if (!strncmp(buffer, "run", cmdlen)) {
			arg = get_arg(buffer, 1);
			// ZEXE-aware: the image size is data + bss, which for the
			// new format is NOT the file size (see sw/common/zexec.h).
			// Legacy raw binaries report bss 0 and total == file size,
			// so this is the same number it always was for them.
			z_exec_info_t xi;
			if (fs_exec_info_any(arg, &xi)) {
				printf("file not found, or not a usable executable\n");
				continue;
			}
			if (!xi.total) {
				printf("file not found/empty\n");
				continue;
			}
			uint32_t size = xi.total;
			printf("creating process (file: %s size: %ld%s)\n", arg,
				(long)size, xi.is_zexe ? "" : " raw");
			fflush(stdout);
			// see kernel.h's z_proc_stack_size_for() comment -- both
			// `repl` and `net` are zport.h providers with a confirmed
			// need for more than the default allowance (per-message
			// zport.h leak, plus repl's own Scheme stdlib loading --
			// see zport.c's own z_port_send() comment).
			// Anything after the name is the program's launch
			// argument (Z_WM_SET_ARG, zwm.h) -- `run view /demo/a.jpg`,
			// `run automate /demo/short.zds` -- joined with spaces, as
			// cron passes a rule's arguments. Held by wm, so it needs
			// wm running; without it the program simply gets none.
			if (get_arg(buffer, 2)) {
				static char run_arg[Z_WM_ARG_MAX];
				uint32_t wmp;
				run_arg[0] = 0;
				for (int ai = 2; ; ai++) {
					char *a = get_arg(buffer, ai);
					if (!a) break;
					if (ai > 2) strncat(run_arg, " ", sizeof(run_arg) - strlen(run_arg) - 1);
					strncat(run_arg, a, sizeof(run_arg) - strlen(run_arg) - 1);
				}
				if (z_pid_lookup("wm0", &wmp)) {
					z_obj_t o;
					o.type = Z_STR;
					o.val.str = run_arg;
					z_msg_new_send(wmp, Z_WM_SET_ARG, 0, o);
					printf("launch argument: %s\n", run_arg);
				} else {
					printf("no wm: launch argument ignored\n");
				}
			}
			uint32_t stack_size = z_proc_stack_size_for(arg);
			uint32_t pid = k_proc_create(size, stack_size);
			printf(" - pid: %ld\n", pid);
			if (!pid) {
				printf("unable to create process\n");
				continue;
			}

			uint32_t base = k_proc_base(pid);
			printf(" - base: %lx\n", base);
			printf(" - loading file\n");
			printf("loading %s from %s\n", arg,
				fs_exec_is_flash(arg) ? "flash" : "sd");
			fs_load_exec_any(base, arg, &xi);
			printf(" - starting process\n");
			k_proc_start(pid);

		}

		// RE-RUN THE INIT SCRIPT
		else if (!strncmp(buffer, "init", cmdlen)) {
			init();	
		}

		// KILL A PROCESS
		else if (!strncmp(buffer, "kill", cmdlen)) {
			arg = get_arg(buffer, 1);
			uint32_t pid;
			if ((!parse_uint(arg, 10, &pid)) || pid == 0) {
				printf("bad pid\n");
				continue;
			}
			printf("killing process %ld: ", pid);
			fflush(stdout);
			if (k_proc_kill(pid) == Z_OK)
				printf("OK\n");
			else
				printf("FAIL\n");
		}

		// CLEAR SCREEN
		else if (!strncmp(buffer, "cls", cmdlen)) {
			cls();
		}

		// SCREENSHOT
		else if (!strncmp(buffer, "ss", cmdlen)) {
			screenshot();
		}

		// DISPLAY PROCESS SNAPSHOT
		else if (!strncmp(buffer, "ps", cmdlen)) {
			k_proc_dump();
		}

		// DISPLAY PID NAME REGISTRY
		else if (!strncmp(buffer, "pr", cmdlen)) {
			k_pidreg_dump();
		}

		// DISPLAY KERNEL SNAPSHOT
		else if (!strncmp(buffer, "ks", cmdlen)) {
			k_kernel_dump();
		}

		// INPUT PATH COUNTERS -- where a keystroke or pointer packet
		// dies between the wire and the app, when one does. The two
		// kernel-side drop points print directly (the HID ring, and
		// sends refused by a full app mailbox); net's half of the
		// story (esp32link's crc errors, fifo overruns and the input
		// events it actually dispatched) is asked for over a message
		// and prints on this same console. All are running totals;
		// what isolates a loss is which of them moves during a burst.
		else if (!strncmp(buffer, "ic", cmdlen)) {
			uint32_t hp, hd;
			k_hid_stats(&hp, &hd);
			printf("ic: hid ring: %lu pushed, %lu dropped (depth %d)\n",
				(unsigned long)hp, (unsigned long)hd, HID_FIFO_SIZE);
			printf("ic: mailboxes: %lu send(s) refused (full, depth %d)\n",
				(unsigned long)k_msg_full_drops(), Z_MAILBOX_DEPTH);
			uint32_t np = resolve_net_pid();
			if (np) {
				z_msg_new_send(np, Z_NET_DEBUG_DUMP, 0, z_obj_none());
				printf("ic: net's counters follow (esp32link dump)\n");
			} else {
				printf("ic: net not running -- its counters unavailable\n");
			}
		}

		// MOUNT / UNMOUNT USB MASS STORAGE AT /usb
		//
		// Explicit rather than automatic on plug. Mounting blocks
		// while the unit reports ready -- a card reader with no card
		// can refuse for a long time -- and the enumeration path runs
		// from an interrupt, so it cannot do this itself.
		else if (!strncmp(buffer, "usbmount", cmdlen)) {
			fs_usb_mount();
		}
		else if (!strncmp(buffer, "usbunmount", cmdlen)) {
			fs_usb_unmount();
		}

#ifdef USBH_DEBUG	// bring-up tools, docs/usb_host.md "Debug build"
		// WIRE CAPTURE OF A FAILING USB TRANSACTION (usbh.h,
		// tools/usbcap.py). `usbcap` arms the logic probe before
		// every transaction on port 0 and freezes it on the first
		// that fails; `usbcapd` prints it for the decoder.
		else if (!strncmp(buffer, "usbcap", cmdlen)) {
			z_usbh_cap_start(0);
		}
		// Same probe, but freezes on the first SUCCESSFUL IN with data
		// from a low-speed device behind a hub, so the window holds the
		// device's packet and our PRE + ACK after it.
		else if (!strncmp(buffer, "usbcapok", cmdlen)) {
			z_usbh_cap_start(1);
		}
		else if (!strncmp(buffer, "usbcapd", cmdlen)) {
			z_usbh_cap_dump();
		}

		// A TERMINAL ON A USB CDC-ACM DEVICE (usb/usbh_cdc.h). Keys
		// go to the device, its output comes here; Ctrl-] leaves.
		// For trying a device out -- apps reach it through the serial
		// port layer once that exists (docs/usb_host.md, "CDC").
		else if (!strncmp(buffer, "usbcdc", cmdlen)) {
			uint8_t rx[64];
			int n, k;
			if (!z_usbh_cdc_present()) {
				printf("usbcdc: no CDC-ACM device\n");
			} else {
				printf("usbcdc: connected; Ctrl-] to leave\n");
				for (;;) {
					if (!k_uart_rx_empty()) {
						int16_t c = k_uart_getc();
						uint8_t b;
						if (c == 0x1d) break;
						b = (uint8_t)c;
						if (z_usbh_cdc_write(&b, 1) < 0) {
							printf("\nusbcdc: write failed\n");
							break;
						}
					}
					n = z_usbh_cdc_read(rx, sizeof(rx));
					if (n < 0) {
						printf("\nusbcdc: device gone\n");
						break;
					}
					for (k = 0; k < n; k++) k_uart_putc((char)rx[k]);
				}
				printf("\nusbcdc: closed\n");
			}
		}

		// Low-speed timings (usb/usbh.h). `usbtune` shows them;
		// `usbtune T R G P` sets timeout, turnaround and gap in us and
		// the J after a PRE in full-speed bit times.
		else if (!strncmp(buffer, "usbtune", 7) &&
			 (buffer[7] == ' ' || buffer[7] == 0)) {
			int v[4] = { 0, 0, 0, 0 }, n = 0;
			char *p = &buffer[7];
			while (*p && n < 4) {
				while (*p == ' ') p++;
				if (!*p) break;
				v[n++] = atoi(p);
				while (*p && *p != ' ') p++;
			}
			if (n == 0) z_usbh_tune_show();
			else if (n == 4) z_usbh_tune_set(v[0], v[1], v[2], v[3]);
			else printf("usage: usbtune [timeout_us turn_us gap_us "
				"pre_bits]\n");
		}

		// Hardware NAK retries for low-speed devices behind a hub
		// (usbh.h). Takes effect on the next transfer.
		else if (!strncmp(buffer, "usbnak", 6) &&
			 (buffer[6] == ' ' || buffer[6] == 0)) {
			z_usbh_set_ls_hub_nak(buffer[6] ? atoi(&buffer[7]) : 0);
		}

#endif

		// LIST USB DEVICES (rtl/usb/, docs/usb_host.md)
		//
		// Named after the tool everyone already knows. Worth more
		// than the one line z_usbh_init() prints at boot, because
		// the interesting state is what changes AFTER boot -- a
		// device plugged in later, a port that enumerated and then
		// dropped, a poll slot whose last status stopped being OK.
		else if (!strncmp(buffer, "lsusb", cmdlen)) {
			z_usbh_dump();
		}

#ifdef USBH_DEBUG	// bring-up tools, docs/usb_host.md "Debug build"
		// RELEASE THE USB PORTS AND READ THE LINES -- isolates
		// "the controller is driving" from "the board is holding
		// a line high", which lsusb alone cannot tell apart.
		else if (!strncmp(buffer, "usbidle", cmdlen)) {
			z_usbh_probe_idle();
		}

		// PACKET BUFFER ROUND-TRIP + THE SETUP BYTES WE WOULD SEND
		else if (!strncmp(buffer, "usbbuf", cmdlen)) {
			z_usbh_buftest();
		}

#endif

		// BUILT-IN LOGIC ANALYSER (rtl/probe.v, docs/probe.md)
		//
		// `probe` arms it and dumps the capture as hex. What it
		// watches is fixed at build time in rtl/sysctl.v -- by
		// default USB host port 0's D+/D-, triggered on our own
		// transmitter, so word 0 is the start of a packet we sent.
		//
		// Absent unless the bitstream was built with `PROBE, which
		// the magic-free CTRL read detects: a build without it
		// decodes this window to csrs and reads back something that
		// is not a plausible word count.
		else if (!strncmp(buffer, "probe", cmdlen)) {
			volatile uint32_t *pc = (volatile uint32_t *)0x7f000000;
			volatile uint32_t *pa = (volatile uint32_t *)0x7f000004;
			volatile uint32_t *pd = (volatile uint32_t *)0x7f000008;
			uint32_t st = *pc;
			uint32_t words = st >> 16;
			int i, guard;

			if (words == 0 || words > 4096) {
				printf("probe: not built in "
					"(rebuild gateware with -DPROBE)\n");
			} else {
				// Arm FIRST, then make traffic happen. The
				// driver gives up after four attempts and goes
				// quiet, so at the shell prompt there is nothing
				// transmitting -- arm-and-wait caught nothing at
				// all, which was the probe working correctly and
				// the experiment being wrong.
				*pc = 1;
				z_usbh_rescan();

				// Enumeration cannot start until the 100 ms
				// attach debounce has run, so wait in kernel
				// ticks (~732 Hz) rather than a spin loop the
				// compiler is entitled to delete.
				{
					uint32_t t0 = z_kernel_ticks;
					while ((z_kernel_ticks - t0) < 2200) {
						st = *pc;
						if (st & 4) break;
					}
				}
				st = *pc;
				if (!(st & 4)) {
					printf("probe: no trigger (ctrl=%08lx)\n",
						(unsigned long)st);
				} else {
					printf("probe: %lu words, 16 samples each, "
						"D+ in the upper bit of each pair\n",
						(unsigned long)words);
					for (i = 0; i < (int)words; i++) {
						*pa = i;
						(void)*pd;
						if ((i & 7) == 0) printf("%04x:", i);
						printf(" %08lx", (unsigned long)*pd);
						if ((i & 7) == 7) printf("\n");
					}
					printf("\n");
				}
			}
		}

		// DISPLAY MEMORY POOL STATS (k_mem_alloc(), sw/os/mem.c) --
		// added to debug a real-hardware "runs out of memory, no
		// error shown" report -- run this after each `run <app>` to
		// see exactly how much is left and whether it's fragmented
		// (see k_mem_dump()'s own comment in mem.c).
		else if (!strncmp(buffer, "free", cmdlen)) {
			k_mem_dump();
		}

		// INSTRUCTION CACHE STATUS / CONTROL (rtl/cache.v)
		//
		// `cache` alone reports hit rate; `cache on|off` is the
		// bring-up escape hatch. Disabling forces every fetch to main
		// memory, exactly as a bitstream built without `ICACHE would,
		// so "is the cache causing this?" can be answered on real
		// hardware in one command rather than a re-synthesis.
		//
		// Counters reset on every flush, and fs_load_exec() flushes on
		// every app load -- so what this reports is activity since the
		// last `run`, not since boot. That's usually what you want when
		// measuring one app, but it's worth knowing before wondering
		// why the numbers look small.
		// CPU / MEMORY MICRO-BENCHMARKS -- see sh_bench() above for what
		// each figure measures and why the boot-time MIPS number isn't
		// enough on its own.
		else if (!strncmp(buffer, "bench", cmdlen)) {
			sh_bench();
		}

		// SD CARD I/O BENCHMARK
		//
		// Separate command from `bench` above rather than another
		// figure inside it: `bench` measures the CPU and memory and
		// costs milliseconds, this one drives real card traffic for a
		// couple of seconds and must not interleave with another
		// process inside FatFs. Nobody should get that by accident
		// from typing `bench`. See docs/sdcard.md.
		else if (!strncmp(buffer, "sdbench", cmdlen)) {
			arg = get_arg(buffer, 1);
			sh_sdbench(arg);
		}

#ifdef USBH_DEBUG	// bring-up tools, docs/usb_host.md "Debug build"
		// The same for USB mass storage (fs/sdbench.c, sh_usbbench()).
		else if (!strncmp(buffer, "usbbench", cmdlen)) {
			arg = get_arg(buffer, 1);
			sh_usbbench(arg);
		}

#endif

		else if (!strncmp(buffer, "cache", cmdlen)) {
			arg = get_arg(buffer, 1);
			sh_cache(arg);
		}

		else if (!strncmp(buffer, "mpu", cmdlen)) {
			arg = get_arg(buffer, 1);
			sh_mpu(arg);
		}

		// GPIO (rtl/gpio.v, docs/gpio.md)
		//
		//   gpio                            report every port
		//   gpio <port> <pin>               read a pin
		//   gpio <port> <pin> in|out|od     set its mode
		//   gpio <port> <pin> 0|1           drive it
		//
		// Ports and pins are both plain numbers, 0-based. There is no
		// letter form: letters in this project mean PMOD connectors
		// (release/hw/boards/*.spec's pmod.a / pmod.b), and a port
		// index is not a connector -- see sw/common/zgpio.h.
		//
		// This lives in the console shell, which is a slightly odd
		// home given that lakritz_gpio has no console at all. It is
		// here for obst_uart_gpio and for bring-up: the first thing
		// anyone does with a new board and a new PMOD is wiggle a pin
		// and look at it with a meter, and that should not require
		// the window manager, an app loader and a working SD card to
		// have come up first. The Scheme API (docs/scheme_api.md)
		// is the one to use from a running desktop.
		//
		// Kernel code, so it calls zgpio.h directly, same as the
		// `color` command below calls zsoc.h -- sh.c IS the kernel.
		else if (!strncmp(buffer, "gpio", cmdlen)) {

			uint32_t nports = z_gpio_port_count();

			if (!z_gpio_present()) {
				// Two different failures, deliberately not merged:
				// a bitstream with no GPIO block at all and one
				// whose ports have no pins need the same fix (a
				// different gateware) but are not the same mistake,
				// and saying which one saves someone checking.
				if (reg_gpio_magic == Z_GPIO_MAGIC)
					printf("gpio: this bitstream has the block but no ports with pins\n");
				else
					printf("gpio: no gpio in this bitstream\n");
				printf("(this is an RTL change -- try `zrelease build obst_uart_gpio`)\n");
			}
			else {

				char *portarg = get_arg(buffer, 1);
				char *pinarg = get_arg(buffer, 2);
				char *valarg = get_arg(buffer, 3);
				uint32_t port = 0, pin = 0;

				if (portarg == NULL) {

					printf("%lu gpio port%s\n", (unsigned long)nports,
						nports == 1 ? "" : "s");

					for (uint32_t i = 0; i < nports; i++)
						printf(" %lu: dir %02x out %02x in %02x\n",
							(unsigned long)i, z_gpio_dir_get(i),
							z_gpio_out_get(i), z_gpio_in_get(i));

					printf("usage: gpio <port> <pin> [in|out|od|0|1]"
						"  e.g. gpio 0 3 out\n");

				}
				else if (!sh_small_num(portarg, &port)
					|| !sh_small_num(pinarg, &pin)) {
					printf("usage: gpio <port> <pin> [in|out|od|0|1]"
						"  e.g. gpio 0 3 out\n");
				}
				else if (port >= nports) {
					// A real port number that this board does not
					// have, which is a different problem from a
					// malformed argument above and gets a different
					// message.
					printf("gpio: no port %lu on this board (have 0..%lu)\n",
						(unsigned long)port,
						(unsigned long)(nports ? nports - 1 : 0));
				}
				else if (pin >= Z_GPIO_PINS_PER_PORT) {
					printf("gpio: no pin %lu (a port has 0..%d)\n",
						(unsigned long)pin, Z_GPIO_PINS_PER_PORT - 1);
				}
				else if (valarg == NULL) {
					printf("%lu.%lu = %d (%s)\n",
						(unsigned long)port, (unsigned long)pin,
						z_gpio_read(port, pin) ? 1 : 0,
						z_gpio_mode_get(port, pin) == Z_GPIO_OUT
							? "output" : "input");
				}
				else if (!strcmp(valarg, "in"))
					z_gpio_mode(port, pin, Z_GPIO_IN);
				else if (!strcmp(valarg, "out"))
					z_gpio_mode(port, pin, Z_GPIO_OUT);
				else if (!strcmp(valarg, "od"))
					z_gpio_mode(port, pin, Z_GPIO_OD);
				else if (!strcmp(valarg, "0") || !strcmp(valarg, "1")) {
					// Drives OUT, not DIR, even on a pin that was set
					// to `od`. There is nowhere in hardware to record
					// that a pin is "open drain" (see zgpio.h), so
					// this command cannot know -- and guessing wrong
					// in the other direction would mean `gpio 0 3 1`
					// silently doing nothing visible. Open-drain
					// bit-banging belongs in a library, not at a
					// prompt.
					z_gpio_write(port, pin, valarg[0] == '1');
				}
				else
					printf("gpio: expected in, out, od, 0 or 1\n");

			}

		}

		// VIRTUAL PHOSPHOR MODE (rtl/socctl.v's VIDEO register)
		//
		// `color` alone reports the current mode; `color <name>` sets
		// it. The four modes are white (white on black, the default),
		// amber, green and paper (black on white).
		//
		// These were `ifdef GPU_AMBER/`ifdef GPU_GREEN in
		// rtl/gpu/gpu_video.v -- chosen at synthesis, changeable only
		// by re-flashing gateware. The defines still exist and still
		// work, but now choose only the power-on default.
		//
		// Kernel code, so this calls zsoc.h's MMIO helpers directly
		// rather than going through the VIDEO_SET_MODE syscall; sh.c
		// IS the kernel and already touches socctl this way for the
		// cursor. Apps use z_video_mode_set() (zeitlos.h) instead.
		else if (!strncmp(buffer, "color", cmdlen)) {

			arg = get_arg(buffer, 1);

			if (!z_video_mode_present()) {
				// Deliberately distinguished from "socctl missing
				// entirely": a bitstream can have socctl and still
				// predate this register, and the fix is the same
				// either way but the diagnosis isn't.
				printf("no video mode register in this bitstream\n");
				printf("(this is an RTL change -- needs `make flash`)\n");
			}
			else if (arg == NULL) {
				printf("color: %s\n", z_video_mode_name(z_video_get_mode()));
				printf("usage: color [white|amber|green|paper]\n");
			}
			else {
				uint32_t mode = z_video_mode_from_name(arg);

				if (mode >= Z_VIDEO_MODE_COUNT) {
					printf("unknown color '%s'\n", arg);
					printf("usage: color [white|amber|green|paper]\n");
				}
				else if (z_video_set_mode(mode)) {
					// Read back rather than echoing `mode`. The write
					// is fire-and-forget on this bus, so reporting the
					// requested value would look identical whether or
					// not it landed -- the same reason wm_busy_apply()
					// reads back after setting the cursor.
					printf("color: %s\n",
						z_video_mode_name(z_video_get_mode()));
				}
				else {
					printf("color: failed to set '%s'\n", arg);
				}
			}
		}

		// DISPLAY FILESYSTEM CAPACITY -- the SD card, as opposed to
		// `free` just above, which is main memory. fs_total()/fs_free()
		// (sw/os/fs/fs.c) have existed since long before this command
		// and were simply never called by anything; both report KB and
		// both return 0 on any failure (no card, not mounted), which is
		// why "not mounted" and "empty" read the same here.
		// FORMAT -- destroys everything on the sdcard.
		//
		// Requires the exact confirmation word as an argument rather
		// than a y/n prompt. A prompt is one keystroke away from
		// wiping a card, and this shell has no undo, no trash and no
		// second copy of anything. Typing "erase-everything" cannot
		// happen by accident or by holding a key down.
		//
		// Deliberately does NOT touch the core apps: those live in
		// flash (sw/os/zar.h) and survive this, so a formatted card
		// still boots to a desktop. That is worth knowing before
		// running it -- the machine will come back up fine.
		else if (!strncmp(buffer, "format", cmdlen)) {

			arg = get_arg(buffer, 1);

			if (arg == NULL || strcmp(arg, "erase-everything") != 0) {
				printf("this will PERMANENTLY ERASE the entire sdcard.\n");
				printf("core apps in flash (wm, net, term) are not\n");
				printf("affected and the system will still boot --\n");
				printf("but repl and posix live on the card and go with it.\n");
				printf("\n");
				printf("to proceed, type exactly:\n");
				printf("  format erase-everything\n");
			}
			else {
				printf("erasing sdcard ...\n");
				if (fs_format() == 0) {
					// f_mkfs leaves the volume unmounted; remount so
					// the very next `ls` or `xf` works instead of
					// failing with FR_NOT_READY.
					if (fs_mount_now() == 0)
						printf("sdcard formatted and remounted.\n");
					else
						printf("formatted, but remount failed -- reboot.\n");
				}
			}

		}

		// Re-mount the SD card.
		//
		// There is no card-detect line on this hardware (SPI only),
		// so nothing can notice a card being inserted or swapped --
		// the volume is mounted once at boot and that is the last
		// word on the subject. This is the manual override: put a
		// card in, type `mount`.
		//
		// It also re-runs disk_initialize(), which is what recovers a
		// card that failed to come up at boot (a slow card, or one
		// inserted a moment too late).
		else if (!strncmp(buffer, "mount", cmdlen)) {

			// Refuse while anything has a file open. Remounting out
			// from under an open FIL leaves that handle describing
			// cluster chains from the previous mount, and the next
			// write through it corrupts the card -- a far worse
			// outcome than making the user close something first.
			int open_now = k_fs_open_count();

			if (open_now) {
				printf("mount: %d file handle(s) still open -- "
					"close them first\n", open_now);
			} else if (fs_mount_now() == 0) {
				uint32_t total = 0, freek = 0;
				fs_df_kb(&total, &freek);
				if (total)
					printf("mounted: %ld KB total, %ld KB free\n",
						(long)total, (long)freek);
				else
					printf("mounted, but no filesystem found\n");
			} else {
				printf("mount failed -- is a card inserted?\n");
			}

		}

		// SYNC -- make the sdcard safe to remove or power off.
		//
		// FatFs holds metadata in RAM: a file's directory entry is not
		// updated until it is closed, and both the volume and every open
		// file carry a 512-byte sector buffer. Cutting power in that
		// window -- or reprogramming the FPGA, which is the same thing
		// from the card's point of view -- leaves lost clusters and
		// half-written directory records.
		//
		// That is the most likely cause of corruption during development,
		// where the board gets reprogrammed far more often than a normal
		// machine gets power-cycled. Run this first and the card is
		// consistent.
		else if (!strncmp(buffer, "sync", cmdlen)) {
			if (fs_unmount() == 0) {
				printf("filesystem flushed; safe to reprogram or remove\n");
				if (fs_mount_now() == 0)
					printf("remounted.\n");
				else
					printf("remount failed -- reboot or reinsert card\n");
			} else {
				printf("flush failed\n");
			}
		}

		// PASSWD, LOCK: docs/security.md
		else if (!strncmp(buffer, "passwd", cmdlen)) {
			k_auth_shell_passwd(get_arg(buffer, 1), k_readline_from_uart);
		}
		else if (!strncmp(buffer, "lock", cmdlen)) {
			k_auth_shell_lock();
		}

		// KV: the flash key/value store, docs/kvstore.md
		else if (!strncmp(buffer, "kv", cmdlen)) {
			char *sub = get_arg(buffer, 1);
			char *a1 = sub ? get_arg(buffer, 2) : NULL;
			char *a2 = a1 ? get_arg(buffer, 3) : NULL;
			k_kv_shell(sub, a1, a2);
		}

		// CONFIGURATION -- /zeitlos.cfg, docs/config.md.
		//   cfg              list known keys (effective values) and
		//                    anything else the file sets
		//   cfg reload       re-read the file after editing it
		//   cfg get <key>    one value
		else if (!strncmp(buffer, "cfg", cmdlen)) {
			char *sub = get_arg(buffer, 1);
			char *key = sub ? get_arg(buffer, 2) : NULL;
			k_cfg_shell(sub, key);
		}

		else if (!strncmp(buffer, "df", cmdlen)) {
			// One FAT scan, not two -- see fs_df_kb() in fs.c.
			uint32_t total = 0, freek = 0;
			fs_df_kb(&total, &freek);
			if (!total) {
				printf("no filesystem mounted\n");
			} else {
				printf(" total: %6ld KB\n", (long)total);
				printf("  used: %6ld KB\n", (long)(total - freek));
				printf("  free: %6ld KB\n", (long)freek);
			}
		}

	}

}

// Loads and starts one app that init() can live without. Nothing is
// returned on purpose: every failure here is reported and survived, and
// a caller that could branch on it is a caller that could stop early.
//
// The memory figure in the failure message is what k_proc_create() was
// asked for. posix's tier is 4MB (Z_PROC_STACK_SIZE_HUGE, kernel.h), so
// on a 1MB or 2MB board "unable to create" is the correct outcome and
// the number says why without anyone having to look it up.
static void init_start_optional(const char *name) {

	printf("starting %s\n", name);

	z_exec_info_t xi;
	core_src_t src = core_exec_info(name, &xi);
	uint32_t size = (src == CORE_SRC_NONE) ? 0 : xi.total;

	if (!size) {
		printf("init: %s not found (non-fatal -- it lives on the sdcard)\n",
			name);
		return;
	}

	uint32_t stack = z_proc_stack_size_for(name);
	uint32_t pid = k_proc_create(size, stack);

	if (!pid) {
		printf("init: unable to create %s process -- needs %luKB "
			"(non-fatal)\n", name,
			(unsigned long)((size + stack + 1023) / 1024));
		return;
	}

	printf("init: %s (%s)\n", name, core_src_name(src));
	core_load_exec(k_proc_base(pid), name, &xi, src);
	k_proc_start(pid);
	printf("init: %s started as pid %ld\n", name, (long)pid);

}

// INIT SCRIPT (hardcoded for now -- see docs/networking.md's
// note on why net/wm need a predictable pid.

void init(void) {

	printf("running init script ...\n");

	if (z_procs[1].base != 0) {
		printf("init: already initialized (pid 1 already reserved)\n");
		return;
	}

	// wm:

	printf("starting wm\n");
	z_exec_info_t xi_wm;
	core_src_t src_wm = core_exec_info("wm", &xi_wm);
	uint32_t size_wm = (src_wm == CORE_SRC_NONE) ? 0 : xi_wm.total;
	if (!size_wm) {
		printf("init: wm binary not found\n");
		return;
	}
	uint32_t pid_wm = k_proc_create(size_wm, z_proc_stack_size_for("wm"));
	if (!pid_wm) {
		printf("init: unable to create wm process\n");
		return;
	}
	uint32_t base_wm = k_proc_base(pid_wm);
	printf("init: wm (%s)\n", core_src_name(src_wm));
	core_load_exec(base_wm, "wm", &xi_wm, src_wm);
	k_proc_start(pid_wm);
	printf("init: wm started as pid %ld\n", pid_wm);

	// net: sw/apps/net -- ARP/ICMP/TFTP/TCP/telnet, see
	// docs/networking.md. Loaded and started normally now, same as
	// wm above -- this used to only reserve net's pid slot (see
	// git history around this comment) because net.c's own startup
	// hung forever on any board without ethernet hardware physically
	// present (e.g. Lakritz, which has only one PMOD slot and it's
	// already occupied by the USB-UART PMOD this console runs over).
	// Fixed: net.c now checks the SOC capability CSRs (rtl/csrs.v,
	// sw/common/zsoc.h, docs/csrs.md) BEFORE touching any ethernet
	// backend register, and exits cleanly (not started, no hang) on a
	// board that confirms it doesn't have the hardware this binary
	// was built for -- so it's now safe to always attempt starting it
	// here, same as any other app. The old reservation dance was also
	// specifically about keeping portdemo's fallback-pid convention
	// correct -- moot now anyway, since portdemo hasn't been started
	// automatically at boot for a while; net's failure here (like
	// repl's and posix's below) is non-fatal to the rest of this
	// script, unlike wm's.

	printf("starting net\n");
	uint32_t pid_net = 0;
	z_exec_info_t xi_net;
	core_src_t src_net = core_exec_info("net", &xi_net);
	uint32_t size_net = (src_net == CORE_SRC_NONE) ? 0 : xi_net.total;
	if (!size_net) {
		printf("init: net binary not found (non-fatal)\n");
	} else {
		pid_net = k_proc_create(size_net, z_proc_stack_size_for("net"));
		if (!pid_net) {
			printf("init: unable to create net process (non-fatal)\n");
		} else {
			uint32_t base_net = k_proc_base(pid_net);
			printf("init: net (%s)\n", core_src_name(src_net));
			core_load_exec(base_net, "net", &xi_net, src_net);
			// started at the end of this function, see there
			printf("init: net loaded as pid %ld\n", pid_net);
		}
	}

	// repl and posix, the two shells a term window connects to, are NOT
	// started here any more: term starts one when its REPL or POSIX
	// button is pressed (docs/terminal.md, "Starting the shells"). A
	// shell nobody asks for costs nothing -- posix alone is 4MB -- and
	// `port repl0` before either button fails, which is fine.
	//
	// This function used to `return` when repl was missing, which also
	// skipped the k_proc_start(pid_net) at the bottom: net was loaded
	// and never ran. Worth remembering for anything added here: optional
	// things are non-fatal and never return early.
	// The kernel console as a port (sw/apps/console, docs/console.md):
	// what term's CONSOLE button connects to. A core app, in flash, so
	// a machine with no card and no serial cable can still see this
	// boot log.
	init_start_optional("console");

	// Look for the shells on the card without starting them. Loading
	// them used to be what brought a freshly powered card up before
	// the config retry below (issue #7): a directory read does the same
	// for the price of a lookup. The answer itself is only reported.
	{
		z_exec_info_t xi;
		if (core_exec_info("repl", &xi) == CORE_SRC_NONE)
			printf("init: repl not found (it lives on the sdcard)\n");
		if (core_exec_info("posix", &xi) == CORE_SRC_NONE)
			printf("init: posix not found (it lives on the sdcard)\n");
	}

	// The config, if boot found no card (issue #7). A freshly powered
	// card can fail its first access and come up moments later -- here,
	// looking the shells up is what brings it up -- so this is the one
	// retry, before anything below reads a setting (tts). If the card is
	// still not up, boot gives up on it: defaults until `cfg reload`.
	k_cfg_retry();

	// Speech, when the machine has been set up for someone who cannot
	// see it (system.tts.enabled, docs/config.md). Started here
	// rather than left to Super+S because a blind user should not
	// have to find a key on an unfamiliar machine to be told anything
	// at all -- and started LAST of the optional apps, so the desktop
	// is already up and there is something to announce.
	//
	// Off by default: the service costs memory and a mixer channel,
	// and a machine nobody has configured should behave as it always
	// has.
	{
		// k_cfg_find(), not z_cfg_get_bool(): the app-side helpers
		// are a syscall, and this is the kernel. cfg.c's own
		// accessor returns the file's value or NULL, so the default
		// (off) is the NULL case.
		const char *v = k_cfg_find("system.tts.enabled");
		if (v && (v[0] == 'y' || v[0] == 'Y' || v[0] == '1' ||
		          v[0] == 't' || v[0] == 'T' || v[0] == 'o' || v[0] == 'O'))
			init_start_optional("tts");
	}

	// cron (docs/cron.md), only if there is something for it to do: a
	// service that sleeps is still RAM. After the config retry above, so
	// the card is up to be asked. It waits for NTP itself when told to
	// (wait_for_ntp), so it does not matter that net starts after it.
	if (fs_size("/user/cron.cfg") > 0)
		init_start_optional("cron");

	// netserve (docs/netserve.md), the same way: only if a service is
	// configured. It waits for net and its address itself.
	{
		static const char *keys[] = { "apps.netserve.ssh", "apps.netserve.telnet",
			"apps.netserve.http", "apps.netserve.echo" };
		for (int i = 0; i < 4; i++) {
			const char *v = k_cfg_find(keys[i]);
			if (v && v[0] && strcmp(v, "off") && strcmp(v, "no")) {
				init_start_optional("netserve");
				break;
			}
		}
	}

	// net is created and loaded above, in its usual slot, but does not
	// start running until every other load is done.
	//
	// Not for the filesystem's sake -- k_no_preempt (kernel.c) settles
	// that. It is what net does on its very first scheduling slice: on
	// a board where the network hardware has to be powered up, that is
	// where it happens, and doing it while the kernel is still
	// streaming an app off the sdcard is what this avoids. On the
	// ULX3S, whose NIC is the onboard ESP32 (docs/esp32link.md),
	// releasing the module mid-load resets the CPU -- consistent with
	// a supply transient, and reproducible: `run net` on its own is
	// fine, and so is loading apps with the radio already up.
	if (pid_net) {
		k_proc_start(pid_net);
		printf("init: net started as pid %ld\n", pid_net);
	}

	// Tells wm that boot has finished loading things.
	//
	// wm keeps its dock disabled (the Z cursor) until this name exists,
	// so nothing is launched from the dock while this function is
	// still streaming repl and posix off the card. It used to wait for
	// repl0 and net0 to register instead, which was really a proxy for
	// "term can connect to repl" -- and would never have cleared on a
	// board without a card (no repl) or without net. See wm.c's
	// init_finished() and docs/window_manager.md.
	//
	// Registered by pid 0, which never exits, so the name is never
	// released.
	{
		char name[Z_PIDREG_NAME_MAX];
		if (z_pid_register("init", name, sizeof(name)))
			printf("init: done (registered %s)\n", name);
		else
			printf("init: done (registering init0 FAILED -- wm will wait for "
				"its timeout before enabling the dock)\n");
	}

}

// returns argument n (0 = command name, 1 = first argument, ...) of
// str, split as a shell splits it -- spaces, and quotes to put a space
// in an argument -- or NULL if there aren't that many.
//
// operates on a private copy internally and hands back a copy of the
// result, rather than using strtok() directly on str and returning a
// pointer into it. strtok() is destructive (writes a '\0' into the
// string at each delimiter it consumes as it goes), and reaching
// argument n means walking past n delimiters internally -- so a
// single call for a HIGH-numbered argument already corrupts the
// buffer for a SUBSEQUENT call asking for a lower-numbered one
// (commands needing more than one argument, like tget/tput, call
// this multiple times per command line; every earlier command only
// ever called it once, which is why this never surfaced before).
// Each result is copied into one of several rotating static slots
// (not a single shared one) so that multiple results from sequential
// calls -- e.g. ip_str/remote/local in tget -- can all still be read
// afterward without one overwriting another.
char *get_arg(char *str, int n) {

	// Quoting, as in the posix shell -- '...', "..." and \x -- so a path
	// can hold a space (sw/common/zargs.h, docs/posix.md "Quoting").
	// Wildcards are NOT expanded here: the kernel stays small, and posix
	// has them. A slot holds a long file name's path (Z_FS_PATH_MAX); it
	// held 64 bytes when a name was 8.3.
	static char slots[8][Z_FS_PATH_MAX];
	static int next_slot = 0;
	static char buf[2 * 256 + 8];
	char *av[16];

	int count = z_args_split(str, buf, sizeof(buf), av, NULL, 16);
	if (n < 0 || n >= count) return NULL;
	z_args_unmark(av[n]);

	char *slot = slots[next_slot];
	next_slot = (next_slot + 1) % 8;

	strncpy(slot, av[n], sizeof(slots[0]) - 1);
	slot[sizeof(slots[0]) - 1] = 0;

	return slot;

}

void hex_dump(uint32_t addr) {

	uint8_t tmp;

	for (int i = 0; i < 16; i++) {
		printf("%.8lx ", addr);
		printf(" ");
		for (int x = 0; x < 16; x++) {
			tmp = (*(volatile uint8_t *)addr);
			printf("%.2x ", tmp);
			addr += 1;
		}
		printf("\n");
	}

}

void cls(void) {
	volatile uint32_t *addr = (uint32_t *)FB_BASE;
	// word count directly -- addr+i is uint32_t* pointer arithmetic
	// (already advances 4 bytes per i), so this must NOT also divide
	// by sizeof(int) the way it used to: (512*384/32)/sizeof(int) was
	// a pre-existing bug that only cleared 1/4 of actual VRAM (1536
	// of the 6144 words the old 512x384 framebuffer actually had).
	// Fixed alongside updating the dimensions themselves for the new
	// native 640x480 resolution (640*480/32 = 9600 words).
	for (int i = 0; i < (FB_SIZE / 4); i++) {
		(*(volatile uint32_t *)(addr + i)) = 0x00000000;
	}
}

// dumps the raw framebuffer, exactly as it sits in VRAM, to ss.bin --
// FB_WIDTH*FB_HEIGHT 1bpp pixels, packed 8 per byte, FB_WIDTH/8 bytes
// per row, no header. See tools/ssconv.py to convert this into a
// viewable PNG.
void screenshot(void) {
	int written = fs_write_file("ss.bin", (char *)FB_BASE, FB_SIZE);
	if (written == FB_SIZE)
		printf("screenshot saved to ss.bin (%d bytes)\n", written);
	else
		printf("ss: write failed (wrote %d of %d bytes)\n", written, FB_SIZE);
}

// hostname/IP resolution for tget/tput above now goes through
// sw/common/zdns.h's z_resolve_host() -- used to be a private
// parse_ipv4() copy here (IP-only, no hostname support), duplicated
// from sw/apps/repl/repl.c's own copy purely because there was
// nowhere shared both build contexts (this kernel build vs. a normal
// app) could reach -- zdns.c's dual-build trick (same one
// sw/common/zstream.c already used, see zdns.c's own header comment)
// finally gave both a real shared home, so both copies were deleted.


// -- micro-benchmarks (`bench`) --
//
// The boot-time MIPS figure (k_cpu_report(), kernel.c) runs one fixed
// integer loop, deliberately unchanged across builds so the number
// stays comparable. That makes it blind to anything it doesn't
// exercise: enabling hardware multiply barely moved it, because that
// loop contains no multiply.
//
// These measure the specific things SOC changes actually affect, in
// CYCLES PER OPERATION, so a change either moves the relevant number
// or it doesn't:
//
//   int   register-only ALU work -- the boot figure's baseline
//   mul   32x32 multiply         -- rtl/boards.vh `CPU_MUL/`CPU_MUL_FAST
//   div   32/32 divide           -- `CPU_DIV
//   ld    sequential word loads  -- main memory read latency
//   ldr   scattered word loads   -- same, defeating row locality
//   st    sequential word stores -- main memory write latency
//
// ld vs ldr is the interesting pair for memory work: an SDRAM
// controller that keeps rows open helps `ld` a lot and `ldr` barely at
// all, so the gap between them is the thing to watch when
// rtl/mem/sdram.v changes.
//
// CAVEAT, measured: ld and ldr currently come out IDENTICAL on both an
// SRAM and an SDRAM board. That is not the controller being perfect,
// it is this benchmark's working set being too small to defeat row
// locality -- buf is 1024 words (4KB), which spans only a couple of
// SDRAM rows, so `(i * 397) & 1023` never leaves the rows already
// open. To actually exercise the row policy this needs a working set
// larger than a few rows; until then, treat ld == ldr as "not
// measured" rather than as evidence that scattering is free.
//
// Everything is `volatile` or consumed into a sink so the compiler
// cannot optimise the work away -- without that, -Os deletes most of
// these loops entirely and reports absurdly fast results.

#define BENCH_ITERS 4096

/* rdcycle and rdinstret are GLOBAL, free-running counters, and the
 * shell is a preemptible process. Without masking, every loop below
 * measures whatever wm, net, repl and the scheduler happened to do
 * inside its window -- not the loop.
 *
 * That is not a small effect, and it silently destroys cross-board
 * comparison. Measured on two boards before this was added: the `int`
 * loop is `x += i; x ^= x >> 7;`, five instructions or so, and it
 * reported 10.72 insn/iter on one board and 20.03 on the other. The
 * same compiled code cannot retire twice the instructions; the gap was
 * background work, and it scales with how long the window happens to
 * be, so the SLOWER board looks disproportionately worse than it is.
 *
 * Masking per-loop rather than around the whole run: each window is
 * ~10ms, which is a long time to be deaf, and this way the system gets
 * to breathe between them. The timer ticks lost will show as a small
 * uptime drift -- the honest cost of measuring a single process on a
 * machine with a shared counter. */
static uint32_t bench_mask;
#define BENCH_BEGIN() (bench_mask = maskirq(0xFFFFFFFF))
#define BENCH_END()   maskirq(bench_mask)

static inline uint32_t bench_cycle(void) {
	uint32_t v; __asm__ volatile ("rdcycle %0" : "=r"(v)); return v;
}

// Instructions retired. rdcycle counts WALL cycles, so it includes
// every cycle spent in other processes while this one was preempted --
// which at 4 runnable processes inflates every figure ~4x and, worse,
// does so unevenly, because each measurement spans only a couple of
// 1.37ms timeslices and the phase relationship shifts between runs.
// That noise is easy to spot: if `mul` comes out lower than `int`,
// the numbers are meaningless, since mul does strictly more work.
//
// rdinstret does NOT fix that, and an earlier version of this comment
// wrongly claimed it did. picorv32's counters are single GLOBAL
// hardware counters -- not virtualised per process, not saved or
// restored across context switches -- so instructions retired by other
// processes are counted here too. Both columns include stolen time.
//
// It is still worth printing, because the two columns divide out: the
// ratio is cycles per instruction, which IS meaningful regardless of
// how much CPU this process got. Comparing insn/op between two loops
// is also fair, since both are inflated by the same factor.
//
// The only clean numbers come from killing the other processes first.
static inline uint32_t bench_instret(void) {
	uint32_t v; __asm__ volatile ("rdinstret %0" : "=r"(v)); return v;
}

// cycles per iteration, x100 so one decimal can be printed without
// floating point (there is none in kernel code)
static uint32_t bench_run_cost(uint32_t cycles, uint32_t iters) {
	if (!iters) return 0;
	return (cycles * 100u) / iters;
}

static void bench_print(const char *name, uint32_t cycles, uint32_t insns,
	uint32_t iters, const char *note) {
	uint32_t c100 = bench_run_cost(cycles, iters);
	uint32_t i100 = bench_run_cost(insns, iters);
	printf("  %-4s %4ld.%02ld cyc  %3ld.%02ld insn   %s\n", name,
		(long)(c100 / 100), (long)(c100 % 100),
		(long)(i100 / 100), (long)(i100 % 100), note);
}

static void sh_bench(void) {

	volatile uint32_t sink = 0;
	uint32_t i, t0, n0, x;
	uint32_t c_int, c_mul, c_div, c_ld, c_ldr, c_st;
	uint32_t n_int, n_mul, n_div, n_ld, n_ldr, n_st;

	// A scratch buffer big enough that scattered access misses
	// whatever row/line the previous access opened. Static rather
	// than on the stack: the shell's stack is not this large.
	static volatile uint32_t buf[1024];

	printf("cycles per operation (lower is better)\n");

	// -- integer ALU --
	x = 12345;
	BENCH_BEGIN();
	n0 = bench_instret(); t0 = bench_cycle();
	for (i = 0; i < BENCH_ITERS; i++) {
		x += i;
		x ^= x >> 7;
	}
	c_int = bench_cycle() - t0; n_int = bench_instret() - n0;
	BENCH_END();
	sink = x;

	// -- multiply --
	x = 12345;
	BENCH_BEGIN();
	n0 = bench_instret(); t0 = bench_cycle();
	for (i = 0; i < BENCH_ITERS; i++) {
		x = x * 1103515245u + 12345u;
	}
	c_mul = bench_cycle() - t0; n_mul = bench_instret() - n0;
	BENCH_END();
	sink = x;

	// -- divide --
	x = 0xffff0000u;
	BENCH_BEGIN();
	n0 = bench_instret(); t0 = bench_cycle();
	for (i = 0; i < BENCH_ITERS; i++) {
		x = x / (i + 3u);
		x += 0x1000u;
	}
	c_div = bench_cycle() - t0; n_div = bench_instret() - n0;
	BENCH_END();
	sink = x;

	// -- sequential loads --
	BENCH_BEGIN();
	n0 = bench_instret(); t0 = bench_cycle();
	for (i = 0; i < BENCH_ITERS; i++) {
		sink = buf[i & 1023];
	}
	c_ld = bench_cycle() - t0; n_ld = bench_instret() - n0;
	BENCH_END();

	// -- scattered loads --
	// 397 is prime relative to 1024, so this walks the whole buffer
	// in a stride that never repeats a nearby address.
	BENCH_BEGIN();
	n0 = bench_instret(); t0 = bench_cycle();
	for (i = 0; i < BENCH_ITERS; i++) {
		sink = buf[(i * 397u) & 1023];
	}
	c_ldr = bench_cycle() - t0; n_ldr = bench_instret() - n0;
	BENCH_END();

	// -- sequential stores --
	BENCH_BEGIN();
	n0 = bench_instret(); t0 = bench_cycle();
	for (i = 0; i < BENCH_ITERS; i++) {
		buf[i & 1023] = i;
	}
	c_st = bench_cycle() - t0; n_st = bench_instret() - n0;
	BENCH_END();

	(void)sink;

	bench_print("int", c_int, n_int, BENCH_ITERS, "add/shift/xor");
	bench_print("mul", c_mul, n_mul, BENCH_ITERS,
		z_soc_has_feature(Z_FEATURE_CPU_MUL) ?
			"hardware" : "software (libgcc)");
	bench_print("div", c_div, n_div, BENCH_ITERS,
		z_soc_has_feature(Z_FEATURE_CPU_DIV) ?
			"hardware" : "software (libgcc)");
	bench_print("ld", c_ld, n_ld, BENCH_ITERS, "sequential word loads");
	bench_print("ldr", c_ldr, n_ldr, BENCH_ITERS, "scattered word loads");
	bench_print("st", c_st, n_st, BENCH_ITERS, "sequential word stores");

	// CPU share: how much of the wall time this process actually got.
	// If it reads well under 100%, every cycles/op figure above is
	// inflated by roughly the reciprocal, and the thing to fix is the
	// scheduler, not the code being measured. Uses the int loop, whose
	// instruction mix is the most predictable.
	printf("\nprocesses: %ld runnable\n", (long)k_proc_runnable_count());
	printf("note: BOTH columns include time/instructions from other\n");
	printf("      processes -- picorv32's counters are global, not per\n");
	printf("      process. cyc/insn (CPI) is still meaningful. For clean\n");
	printf("      absolute numbers, kill the other processes first.\n");

}

void sh_help(void) {

	printf("commands:\n");
	printf(" hd <addr>         hex dump memory\n");
	printf(" reboot            reconfigure the FPGA, after syncing files\n");
	printf(" jump [addr]       the jumploader; with a hex address, boot from it\n");
	printf(" flash             the flash: ID, size, lock, status\n");
	printf(" flashtest         test erase/program on sector 0x1FC000\n");
	printf(" kv [get|set|del|compact|test] ...  the flash key/value store\n");
	printf(" passwd [reset]    set, change or remove the password\n");
	printf(" lock              lock the screen (and the console, if set to)\n");
	printf(" probe             dump logic analyser capture "
		"(needs -DPROBE)\n");
	printf(" usbmount          mount usb storage at /usb\n");
	printf(" usbunmount        unmount /usb\n");
#ifdef USBH_DEBUG
	printf(" usbcapok          capture a good low-speed IN + our ACK\n");
#endif
#ifdef USBH_DEBUG
	printf(" usbcap            capture the next failing usb transaction "
		"(needs -DPROBE)\n");
#endif
#ifdef USBH_DEBUG
	printf(" usbcapd           print that capture for tools/usbcap.py\n");
#endif
#ifdef USBH_DEBUG
	printf(" usbnak N          hw NAK retries, hubs and behind them (0)\n");
#endif
#ifdef USBH_DEBUG
	printf(" usbtune [T R G P] low-speed timeout/turn/gap us, PRE gap bits\n");
#endif
#ifdef USBH_DEBUG
	printf(" usbcdc            terminal on a usb cdc-acm device (^] exits)\n");
#endif
	printf(" lsusb             usb devices and port state\n");
#ifdef USBH_DEBUG
	printf(" usbidle           release usb ports, read raw lines\n");
#endif
#ifdef USBH_DEBUG
	printf(" usbbuf            usb packet buffer round-trip test\n");
#endif
	printf(" xa <addr>         receive to addr via xfer\n");
	printf(" xf <file>         receive to file via xfer\n");
	printf(" xmf <file>        receive to file via xmodem\n");
	printf(" tget <ip-or-host> <remote-file> [local-file]  fetch a file via tftp (needs `run net`)\n");
	printf(" tput <ip-or-host> <local-file> [remote-file]  send a file via tftp (needs `run net`)\n");
	printf(" run <file> [arg]  create a new process; arg is its launch argument\n");
	printf(" init               start wm, net, repl and posix (runs at boot)\n");
	printf(" cfg [reload|get k] show or re-read /zeitlos.cfg (docs/config.md)\n");
	printf(" kill <pid>        kill a process\n");
	printf(" ps                display a process snapshot\n");
	printf(" df                display filesystem capacity\n");
	printf(" sync              flush sdcard (before reprogramming)\n");
	printf(" mount             (re)mount the sdcard -- no card-detect, so this is manual\n");
	printf(" format            ERASE the entire sdcard\n");
	printf(" pr                display the pid name registry\n");
	printf(" ks                display a kernel snapshot\n");
	printf(" ic                input path counters (hid ring, mailboxes, esp32link)\n");
	printf(" cls               clear framebuffer\n");
	printf(" ss                save a screenshot to ss.bin (see tools/ssconv.py)\n");
	printf(" ls [path]         display list of files\n");
	printf(" mkdir [path]      make a directory\n");
	printf(" touch [path]      create empty file\n");
	printf(" rm [path]         remove a file\n");
	printf(" cache [on|off|flush|don|doff|wbon|wboff|clear]  cache stats/control\n");
	printf(" mpu [report|enforce|off|clear]  memory protection\n");
	printf(" color [white|amber|green|paper]  display phosphor mode\n");
	printf(" gpio [port] [pin] [in|out|od|0|1]  read/drive gpio pins (e.g. gpio 0 3 out)\n");
	printf(" bench             cpu/memory micro-benchmarks\n");
	printf(" sdbench [file]    layered sdcard throughput benchmark (docs/sdcard.md)\n");
#ifdef USBH_DEBUG
	printf(" usbbench [file]   the same for usb storage (docs/usb_host.md)\n");
#endif

}

// -- the flash: `flash` and `flashtest` (docs/spiflash.md) ---------------

static void sh_flash_info(void) {
	if (!k_flash_present()) {
		printf("flash: not writable on this bitstream (FEATURES2 bit 6 clear)\n");
		return;
	}
	uint32_t st, n = 0;
	while (((st = k_flash_hw_status()) & Z_SPIFLASH_BUSY) && n < 3000000u) n++;
	uint32_t id = *(volatile uint32_t *)(Z_SPIFLASH_BASE + Z_SPIFLASH_ID);
	uint32_t cap = id & 0xFF;
	printf("flash: JEDEC %02lx %02lx %02lx", (unsigned long)((id >> 16) & 0xFF),
		(unsigned long)((id >> 8) & 0xFF), (unsigned long)cap);
	if (cap >= 16 && cap <= 28) printf(", %lu KB", (unsigned long)((1ul << cap) / 1024));
	printf("\n       locked below 0x%06lx; status %08lx%s\n",
		(unsigned long)*(volatile uint32_t *)(Z_SPIFLASH_BASE + Z_SPIFLASH_LOCK),
		(unsigned long)st, k_flash_session_active() ? "; a write session is open" : "");
}

/* Below the key/value store (docs/kvstore.md), which has the last 8 KB
 * of the chip, and past every jumploader built for a 2 MB flash (the
 * 45F one ends at 0x1F7BE9). On a larger flash this is inside an 85F
 * jumploader -- which is what the blank-or-ours check below is for, as
 * it was at the old 0x1FF000. */
#define FT_SECTOR 0x1FC000u
#define FT_SIG    "ZFLASHTEST"

static volatile const uint8_t *ft_win(uint32_t off) {
	return (volatile const uint8_t *)(0x10000000u + off);
}

static uint8_t ft_byte(uint32_t i) {
	return (uint8_t)(i * 37u + 11u);
}

// Bounded: a 4 KB erase is 400 ms at worst, and a controller that never
// clears busy is what this test exists to report, not to hang on.
static int ft_wait(uint32_t *ticks) {
	uint32_t t0 = z_kernel_ticks, st, n = 0;
	while (((st = k_flash_hw_status()) & Z_SPIFLASH_BUSY) && n < 3000000u) n++;
	if (ticks) *ticks = z_kernel_ticks - t0;
	if (st & Z_SPIFLASH_BUSY) {
		printf("  (the controller still reports busy: status %08lx)\n", (unsigned long)st);
		return -2;
	}
	return (st & Z_SPIFLASH_DONE) ? 0 : -1;
}

static void sh_flashtest(void) {
	int fails = 0;
	uint32_t i, st, ticks;
	static uint8_t page[256];
	#define FT(ok, what) do { if (ok) printf("  ok   %s\n", what); \
		else { printf("  FAIL %s\n", what); fails++; } } while (0)

	if (!k_flash_present()) {
		printf("flashtest: the flash is not writable on this bitstream\n");
		return;
	}
	ft_wait(NULL);

	if (k_kv_overlaps(FT_SECTOR, 4096)) {
		printf("flashtest: sector 0x%06lx is the key/value store on this flash; "
			"not touching it\n", (unsigned long)FT_SECTOR);
		return;
	}

	// Only a blank sector, or one this test wrote, is erased.
	bool blank = true, ours = true;
	for (i = 0; i < 4096; i++) if (ft_win(FT_SECTOR)[i] != 0xFF) { blank = false; break; }
	for (i = 0; i < sizeof(FT_SIG) - 1; i++) if (ft_win(FT_SECTOR)[i] != (uint8_t)FT_SIG[i]) ours = false;
	if (!blank && !ours) {
		printf("flashtest: sector 0x%06lx holds data this test did not write; "
			"not touching it\n", (unsigned long)FT_SECTOR);
		return;
	}
	if (!k_flash_begin(K_FLASH_KERNEL)) {
		printf("flashtest: another program has a flash write session open\n");
		return;
	}
	printf("flashtest: sector 0x%06lx (%s)\n", (unsigned long)FT_SECTOR,
		blank ? "blank" : "from an earlier run");

	// 1. refusals, from the controller itself: nothing reaches the flash
	*(volatile uint32_t *)(Z_SPIFLASH_BASE + Z_SPIFLASH_ADDR) = FT_SECTOR;
	*(volatile uint32_t *)(Z_SPIFLASH_BASE + Z_SPIFLASH_ARM) = 0;        // disarm
	*(volatile uint32_t *)(Z_SPIFLASH_BASE + Z_SPIFLASH_CMD) = Z_SPIFLASH_CMD_ERASE;
	st = k_flash_hw_status();
	FT((st & Z_SPIFLASH_E_UNARMED) && !(st & Z_SPIFLASH_BUSY), "an unarmed erase is refused");
	FT(k_flash_hw_erase(0x000000) == Z_SPIFLASH_E_LOCKED, "erasing sector 0 (the bootloader) is refused");
	FT(k_flash_hw_erase(0x03F000) == Z_SPIFLASH_E_LOCKED, "erasing 0x03F000 (the lock's last sector) is refused");
	for (i = 0; i < 16; i++) page[i] = 0;
	FT(k_flash_hw_program(0x03FF00, page, 16) == Z_SPIFLASH_E_LOCKED, "programming 0x03FF00 is refused");

	// 2. erase
	FT(k_flash_hw_erase(FT_SECTOR) == 0, "erase started");
	FT(ft_wait(&ticks) == 0, "erase done");
	printf("       (%lu ms)\n", (unsigned long)(ticks * 1000u / 732u));
	blank = true;
	for (i = 0; i < 4096; i++) if (ft_win(FT_SECTOR)[i] != 0xFF) { blank = false; break; }
	FT(blank, "the sector reads blank");

	// 3. a whole page, signed, then an odd, unaligned run
	for (i = 0; i < 256; i++) page[i] = ft_byte(i);
	for (i = 0; i < sizeof(FT_SIG) - 1; i++) page[i] = (uint8_t)FT_SIG[i];
	FT(k_flash_hw_program(FT_SECTOR, page, 256) == 0, "program 256 bytes started");
	FT(ft_wait(NULL) == 0, "program done");
	bool same = true;
	for (i = 0; i < 256; i++) if (ft_win(FT_SECTOR)[i] != page[i]) { same = false; break; }
	FT(same, "256 bytes read back through the window");

	for (i = 0; i < 13; i++) page[i] = ft_byte(1000 + i);
	FT(k_flash_hw_program(FT_SECTOR + 0x105, page, 13) == 0, "program 13 bytes at +0x105 started");
	FT(ft_wait(NULL) == 0, "program done");
	same = true;
	for (i = 0; i < 13; i++) if (ft_win(FT_SECTOR + 0x105)[i] != page[i]) same = false;
	if (ft_win(FT_SECTOR + 0x104)[0] != 0xFF || ft_win(FT_SECTOR + 0x112)[0] != 0xFF) same = false;
	FT(same, "13 bytes read back, and their neighbours untouched");

	k_flash_end(K_FLASH_KERNEL);
	printf("flashtest: %s\n", fails ? "FAILED -- please report the lines above" : "passed");
	#undef FT
}
