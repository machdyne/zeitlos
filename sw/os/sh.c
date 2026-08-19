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
#include "kernel.h"
#include "fs/fs.h"
#include "fs/fatfs/ff.h"
#include "msg.h"
#include "pidreg.h"

// --

char *get_arg(char *str, int n);
void sh_help(void);
void hex_dump(uint32_t addr);
uint32_t xfer_recv(uint32_t addr_ptr);
void cls(void);
bool parse_ipv4(const char *s, uint32_t *out);
void init(void);

// shortened for now (was 60s) while TFTP is still being brought up --
// waiting a full minute per failed attempt makes debugging painfully
// slow. 10s is still generous for a local network exchange; raise it
// back once TFTP is confirmed working, since a real large-file
// transfer could plausibly need longer.
#define TFTP_REPLY_TIMEOUT_TICKS (10 * 732)

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

static uint32_t resolve_net_pid(void) {
	if (!net_pid_resolved) {
		if (!z_pid_lookup("net0", &net_pid_cache))
			net_pid_cache = Z_PID_NET;
		net_pid_resolved = true;
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
			void *tmp = k_mem_alloc(1024*256); // 256K max file size for now
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

		// GET FILE VIA TFTP (uses the 'net' app -- see sw/common/znet.h)
		else if (!strncmp(buffer, "tget", cmdlen)) {

			char *ip_str = get_arg(buffer, 1);
			char *remote = get_arg(buffer, 2);
			char *local = get_arg(buffer, 3);

			if (!ip_str || !remote) {
				printf("usage: tget <server-ip> <remote-file> [local-file]\n");
				continue;
			}
			if (!local) local = remote;

			uint32_t ip;
			if (!parse_ipv4(ip_str, &ip)) {
				printf("tget: bad ip address\n");
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
			char err[64];
			if (!zstream_open(&cons, resolve_net_pid(), req, err, sizeof(err))) {
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
				printf("usage: tput <server-ip> <local-file> [remote-file]\n");
				continue;
			}
			if (!remote) remote = local;

			uint32_t ip;
			if (!parse_ipv4(ip_str, &ip)) {
				printf("tput: bad ip address\n");
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
			z_msg_new_send(resolve_net_pid(), Z_NET_TFTP_PUT, tag, req);
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
			uint32_t size = fs_size(arg);
			if (!size) {
				printf("file not found/empty\n");
				continue;
			}
			printf("creating process (file: %s size: %ld)\n", arg, size);
			fflush(stdout);
			uint32_t pid = k_proc_create(size);
			printf(" - pid: %ld\n", pid);
			if (!pid) {
				printf("unable to create process\n");
				continue;
			}

			uint32_t base = k_proc_base(pid);
			printf(" - base: %lx\n", base);
			printf(" - loading file\n");
			fs_load(base, arg);
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
	uint32_t size_wm = fs_size("wm");
	if (!size_wm) {
		printf("init: wm binary not found\n");
		return;
	}
	uint32_t pid_wm = k_proc_create(size_wm);
	if (!pid_wm) {
		printf("init: unable to create wm process\n");
		return;
	}
	uint32_t base_wm = k_proc_base(pid_wm);
	fs_load(base_wm, "wm");
	k_proc_start(pid_wm);
	printf("init: net started as pid %ld\n", pid_wm);

	// net:
	//
	// NOT started here, only its pid slot reserved -- net.c's own
	// startup currently hangs forever when there's no
	// NIC PMOD physically present, which is always true on Lakritz
	// right now: it only has one PMOD slot, and that's already
	// occupied by the USB-UART PMOD this console runs over. Reserving
	// the slot without loading/starting the binary (k_proc_create()
	// only -- no fs_load(), no k_proc_start()) still gets net its
	// usual pid (2), which is all portdemo below actually needs.
	//
	// this whole reservation dance is now specifically about keeping
	// the FALLBACK path correct, not the primary one: term.c/zport.h
	// prefer looking up "portdemo0" by name (sw/os/pidreg.h) these
	// days, which doesn't care what pid portdemo actually lands on --
	// but if that lookup ever fails (registry full, or portdemo
	// somehow started before it could register), the fallback is
	// still the fixed Z_PID_PORTDEMO constant, and THAT still depends
	// on wm/net/portdemo being created in this same fixed order every
	// time (see docs/networking.md's note on why net/wm need a
	// predictable pid, and docs/ports.md's "Testing this"). Worth
	// keeping even though it's now a belt-and-suspenders fallback
	// rather than the only thing standing between term and a wrong
	// pid.
	//
	// once net's NIC-detection hang is fixed (or on a board/config
	// that actually has a NIC PMOD available), this can go back to
	// loading+starting net normally -- or, in the meantime, `run net`
	// still works to start it manually (on a DIFFERENT, newly
	// allocated pid, not this reserved one -- fine for manual testing,
	// since it'll still register as "net0" and be found by name
	// either way; only something still relying on the Z_PID_NET
	// fallback specifically would need the reserved pid).

	printf("reserving net's pid (not starting it -- see comment above)\n");
	uint32_t size_net = fs_size("net");
	if (!size_net) {
		printf("init: net binary not found\n");
		return;
	}
	uint32_t pid_net = k_proc_create(size_net);
	if (!pid_net) {
		printf("init: unable to reserve net process\n");
		return;
	}
	printf("init: net pid %ld reserved (not started)\n", pid_net);

	// portdemo: a virtual port provider with no real hardware behind
	// it -- see docs/ports.md and sw/apps/portdemo/portdemo.c. same
	// reservation reasoning as wm/net above, for the fallback path --
	// term.c prefers looking up "portdemo0" by name now, but falls
	// back to the fixed pid Z_PID_PORTDEMO (zport.h) if that lookup
	// fails, so it's still worth landing here predictably.
	// not fatal if this one specifically fails to start (unlike
	// wm/net above) -- term falls back to local echo without it, see
	// term.c's own header comment.

	printf("starting portdemo\n");
	uint32_t size_portdemo = fs_size("portdemo");
	if (!size_portdemo) {
		printf("init: portdemo binary not found (non-fatal -- term will "
			"fall back to local echo)\n");
		return;
	}
	uint32_t pid_portdemo = k_proc_create(size_portdemo);
	if (!pid_portdemo) {
		printf("init: unable to create portdemo process (non-fatal)\n");
		return;
	}
	uint32_t base_portdemo = k_proc_base(pid_portdemo);
	fs_load(base_portdemo, "portdemo");
	k_proc_start(pid_portdemo);
	printf("init: portdemo started as pid %ld\n", pid_portdemo);

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
	volatile uint32_t *addr = (uint32_t *)0x20000000;
	// word count directly -- addr+i is uint32_t* pointer arithmetic
	// (already advances 4 bytes per i), so this must NOT also divide
	// by sizeof(int) the way it used to: (512*384/32)/sizeof(int) was
	// a pre-existing bug that only cleared 1/4 of actual VRAM (1536
	// of the 6144 words the old 512x384 framebuffer actually had).
	// Fixed alongside updating the dimensions themselves for the new
	// native 640x480 resolution (640*480/32 = 9600 words).
	for (int i = 0; i < ((640 * 480) / 32); i++) {
		(*(volatile uint32_t *)(addr + i)) = 0x00000000;
	}
}

// parses a dotted-quad IPv4 address ("a.b.c.d") into a packed
// uint32_t (matching znet.h's convention -- same packing z_map_find'd
// "ip" values use, e.g. ip.c's own address handling). returns false
// on a malformed address rather than silently returning 0
// (0.0.0.0 is itself a value someone could plausibly type by mistake,
// so treating a parse failure as "0" would be a silent, misleading
// success).
bool parse_ipv4(const char *s, uint32_t *out) {

	uint32_t octets[4];

	for (int i = 0; i < 4; i++) {

		if (i > 0) {
			if (*s != '.') return false;
			s++;
		}

		if (*s < '0' || *s > '9') return false;

		uint32_t v = 0;
		int digits = 0;
		while (*s >= '0' && *s <= '9') {
			v = v * 10 + (*s - '0');
			s++;
			digits++;
			if (digits > 3 || v > 255) return false;
		}

		octets[i] = v;

	}

	if (*s != '\0') return false;	// trailing garbage

	*out = (octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) | octets[3];
	return true;

}

void sh_help(void) {

	printf("commands:\n");
	printf(" hd <addr>         hex dump memory\n");
	printf(" xa <addr>         receive to addr via xfer\n");
	printf(" xf <file>         receive to file via xfer\n");
	printf(" tget <ip> <remote-file> [local-file]  fetch a file via tftp (needs `run net`)\n");
	printf(" tput <ip> <local-file> [remote-file]  send a file via tftp (needs `run net`)\n");
	printf(" run <file>        create a new process\n");
	printf(" init               reserve pid 1, start net as pid 2 (no wm needed)\n");
	printf(" kill <pid>        kill a process\n");
	printf(" ps                display a process snapshot\n");
	printf(" pr                display the pid name registry\n");
	printf(" ks                display a kernel snapshot\n");
	printf(" cls               clear framebuffer\n");
	printf(" ls [path]         display list of files\n");
	printf(" mkdir [path]      make a directory\n");
	printf(" touch [path]      create empty file\n");
	printf(" rm [path]         remove a file\n");

}
