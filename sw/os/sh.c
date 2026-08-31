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
#include "../common/znet.h"
#include "../common/zstream.h"
#include "../common/zdns.h"
#include "kernel.h"
#include "mem.h"
#include "uart.h"
#include "zar.h"
#include "../common/zsoc.h"	// Z_TICK_HZ	// k_uart_getc()/k_uart_rx_empty() -- see
					// boot_cancel_requested() below
#include "fs/fs.h"
#include "fsapi.h"
#include "fs/fatfs/ff.h"
#include "msg.h"
#include "pidreg.h"
#include "xmodem.h"

// --

char *get_arg(char *str, int n);
void sh_help(void);
static void sh_bench(void);
void hex_dump(uint32_t addr);
uint32_t xfer_recv(uint32_t addr_ptr);
void cls(void);
void init(void);
void screenshot(void);

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
	if (z_zar_present())
		card_ready = (fs_mount_now() == 0);
	else
		card_ready = wait_for_card_ready();

	if (card_ready)
		printf("init: sdcard ready\n");
	else if (z_zar_present())
		printf("init: no sdcard, using core apps in flash\n");

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

		// HEX DUMP
		else if (!strncmp(buffer, "hd", cmdlen)) {
			arg = get_arg(buffer, 1);
			uint32_t addr;
			if (sscanf(arg, "%lx", &addr))
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
			if (!sscanf(arg, "%lx", &addr)) {
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
			if ((!sscanf(arg, "%ld", &pid)) || pid == 0) {
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

		else if (!strncmp(buffer, "cache", cmdlen)) {
			arg = get_arg(buffer, 1);

			if (!z_icache_present()) {
				printf("no instruction cache in this bitstream\n");
			}
			else if (arg != NULL && !strcmp(arg, "on")) {
				z_icache_enable(true);
				printf("instruction cache enabled\n");
			}
			else if (arg != NULL && !strcmp(arg, "off")) {
				z_icache_enable(false);
				printf("instruction cache disabled\n");
			}
			else if (arg != NULL && !strcmp(arg, "flush")) {
				z_icache_flush();
				printf("instruction cache flushed\n");
			}
			else {
				uint32_t hits = reg_icache_hits;
				uint32_t misses = reg_icache_misses;
				uint32_t total = hits + misses;
				uint32_t h = hits;
				uint32_t t = total;
				uint32_t permille;

				// scale both down together before multiplying by
				// 1000, rather than reaching for 64-bit math: h*1000
				// overflows a uint32_t past ~4.29M hits, which is a
				// perfectly reachable count between two flushes.
				while (t > 4000000u) { t >>= 4; h >>= 4; }
				permille = t ? (h * 1000u) / t : 0;

				printf("icache: %ldKB, %ld-word lines%s\n",
					z_icache_kb(), z_icache_line_words(),
					(reg_icache_ctrl & Z_ICACHE_CTRL_ENABLE) ?
						"" : " (DISABLED)");
				printf("  hits:   %ld\n", hits);
				printf("  misses: %ld\n", misses);
				if (total)
					printf("  rate:   %ld.%ld%%\n",
						permille / 10, permille % 10);
				else
					printf("  rate:   (no fetches yet)\n");
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
				printf("core apps in flash (wm, net, repl, term) are not\n");
				printf("affected and the system will still boot.\n");
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
	// wm/repl above -- this used to only reserve net's pid slot (see
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
	// automatically at boot for a while (see the `repl` comment
	// below); net's failure here (like repl's) is non-fatal to the
	// rest of this script, unlike wm's.

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

	// repl: Zeitlos's command interpreter -- see
	// sw/apps/repl/repl.c. same reservation reasoning as wm/net
	// above, for the fallback path -- term.c prefers looking up
	// "repl0" by name now, but falls back to the fixed pid
	// Z_PID_REPL (zrepl.h) if that lookup fails, so it's still worth
	// landing here predictably. not fatal if this one specifically
	// fails to start (unlike wm above) -- term falls back to
	// local echo without it, see term.c's own header comment.
	//
	// this replaces starting portdemo here (see docs/ports.md and
	// sw/apps/portdemo/portdemo.c) -- term no longer looks for
	// portdemo by default (see term.c's own header comment on why),
	// so starting it automatically at boot no longer serves the
	// purpose this reservation dance exists for. portdemo itself is
	// unchanged and still builds/runs fine manually (`run portdemo`)
	// for testing the port protocol in isolation from repl.

	printf("starting repl\n");
	z_exec_info_t xi_repl;
	core_src_t src_repl = core_exec_info("repl", &xi_repl);
	uint32_t size_repl = (src_repl == CORE_SRC_NONE) ? 0 : xi_repl.total;
	if (!size_repl) {
		printf("init: repl binary not found (non-fatal -- term will "
			"fall back to local echo)\n");
	} else {
		uint32_t pid_repl = k_proc_create(size_repl, z_proc_stack_size_for("repl"));
		if (!pid_repl) {
			printf("init: unable to create repl process (non-fatal)\n");
		} else {
			uint32_t base_repl = k_proc_base(pid_repl);
			printf("init: repl (%s)\n", core_src_name(src_repl));
			core_load_exec(base_repl, "repl", &xi_repl, src_repl);
			k_proc_start(pid_repl);
			printf("init: repl started as pid %ld\n", pid_repl);
		}
	}

	// net is started last, once every load above has finished. FatFs
	// access is serialised by fs_lock() (sw/os/fs/fs.c) now, so this is
	// no longer needed for correctness; it still keeps net's first act
	// (reading NET.CFG, then releasing the ESP32 on the ULX3S) from
	// competing with the last load for the card, and keeps Z_PID_NET
	// where it always was: net is created and loaded in its usual slot
	// above, only the moment it starts running moves.
	if (pid_net) {
		k_proc_start(pid_net);
		printf("init: net started as pid %ld\n", pid_net);
	}

}

// returns argument n (0 = command name, 1 = first argument, ...) of
// str, split on spaces, or NULL if there aren't that many.
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

	static char slots[8][64];
	static int next_slot = 0;

	char tmp[256];
	strncpy(tmp, str, sizeof(tmp) - 1);
	tmp[sizeof(tmp) - 1] = 0;

	char *token = strtok(tmp, " ");
	int tn = 0;

	while (token != NULL && tn != n) {
		token = strtok(NULL, " ");
		tn++;
	}

	if (!token) return NULL;

	char *slot = slots[next_slot];
	next_slot = (next_slot + 1) % 8;

	strncpy(slot, token, sizeof(slots[0]) - 1);
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
	printf(" xa <addr>         receive to addr via xfer\n");
	printf(" xf <file>         receive to file via xfer\n");
	printf(" xmf <file>        receive to file via xmodem\n");
	printf(" tget <ip-or-host> <remote-file> [local-file]  fetch a file via tftp (needs `run net`)\n");
	printf(" tput <ip-or-host> <local-file> [remote-file]  send a file via tftp (needs `run net`)\n");
	printf(" run <file>        create a new process\n");
	printf(" init               start wm, net, and repl (runs automatically at boot)\n");
	printf(" kill <pid>        kill a process\n");
	printf(" ps                display a process snapshot\n");
	printf(" df                display filesystem capacity\n");
	printf(" sync              flush sdcard (before reprogramming)\n");
	printf(" mount             (re)mount the sdcard -- no card-detect, so this is manual\n");
	printf(" format            ERASE the entire sdcard\n");
	printf(" pr                display the pid name registry\n");
	printf(" ks                display a kernel snapshot\n");
	printf(" cls               clear framebuffer\n");
	printf(" ss                save a screenshot to ss.bin (see tools/ssconv.py)\n");
	printf(" ls [path]         display list of files\n");
	printf(" mkdir [path]      make a directory\n");
	printf(" touch [path]      create empty file\n");
	printf(" rm [path]         remove a file\n");
	printf(" cache [on|off|flush]  instruction cache stats/control\n");
	printf(" color [white|amber|green|paper]  display phosphor mode\n");
	printf(" bench             cpu/memory micro-benchmarks\n");

}
