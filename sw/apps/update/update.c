/*
 * update -- bring the card up to date with a release (docs/update.md).
 *
 *   update                              what the card has
 *   update v0.0.7                       from the distribution's server, over
 *                                       HTTP, signed (docs/distribution.md)
 *   update latest                       whatever that server's LATEST names
 *   update v0.0.7 -s games              ... adding the games set
 *   update v0.0.7 -t http URL           from another server
 *   update v0.0.7 -t tftp 192.168.1.10  from a TFTP server (unsigned: a LAN)
 *   update latest -t tftp server        whatever the server's LATEST names
 *   update -n v0.0.7 -t tftp server     what would change; changes nothing
 *   update -v v0.0.7 -t tftp server     hash every file, trust no manifest
 *   update -f v0.0.7 -t tftp server     though the release's ABI is not the
 *                                       flash's (docs/update.md, "ABI")
 *   update ... -d DIR                   the store is DIR/ on the server,
 *                                       not release/
 *
 * Run from posix (`update v0.0.7 -t tftp ...`, or `run update ...`) its
 * output is in that terminal; from the kernel console or repl, on the
 * console. Stop it with `kill update0` (posix), `kill <pid>`, or from
 * info: it is safe to stop at any moment, and running it again
 * finishes the job.
 *
 * This file binds upd.c, which does the work and is tested on the host
 * (tests/), to the card (fs, through zfsapp), the network (net's TFTP
 * client, as a stream) and the terminal.
 *
 * -- one message loop --
 *
 * zstream_pull() and z_port_send()'s callers each read the mailbox
 * waiting for their own reply and throw away whatever else arrives.
 * Here both run at once -- chunks arriving from net while output goes
 * to posix and its acks come back -- so neither may: everything is
 * read in upd_net_pump() (upd_net.c), which queues stream events and
 * hands the rest (port acks) to on_other() here.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"           /* Z_TICK_HZ */
#include "../../common/zport.h"
#include "../../common/zstream.h"
#include "../../common/zdns.h"
#include "../../common/zfsapp.h"
#include "../../common/zwin.h"          /* z_launch_arg_take() */
#include "../../common/zargs.h"
#include "../../common/zcfg.h"
#include "../posix/posix.h"             /* PX_STDOUT_TAG */

#include "upd.h"
#include "upd_net.h"
#include "zdist.h"                      /* the built-in distribution */
#include "../../common/zpaths.h"

void uart_putc(char c);

#define WAIT_TICKS      (Z_TICK_HZ / 50)        // 20 ms naps

// -- output: posix's terminal, or the console ------------------------------

static z_port_t out;
static bool out_on;

static void out_raw(const char *b, int n) {
	if (!out_on) {
		for (int i = 0; i < n; i++) uart_putc(b[i]);
		return;
	}
	// At most 8 sends unacknowledged (zport.h); wait for room, reading
	// the mailbox through upd_net_pump() so a chunk arriving meanwhile
	// is kept.
	uint32_t t0 = z_uptime_ticks();
	while (z_port_send(&out, b, (uint32_t)n) != Z_OK) {
		if (!out_on || z_port_peer_gone(&out) ||
				z_uptime_ticks() - t0 > Z_TICK_HZ * 5) {
			out_on = false;		// the terminal went: the console, then
			for (int i = 0; i < n; i++) uart_putc(b[i]);
			return;
		}
		upd_net_pump(WAIT_TICKS);
	}
}

static void io_out(void *c, const char *t) {
	(void)c;
	char b[512];
	int n = 0;
	for (; *t; t++) {
		if (n > (int)sizeof b - 3) {
			out_raw(b, n);
			n = 0;
		}
		if (*t == '\n') b[n++] = '\r';
		b[n++] = *t;
	}
	if (n) out_raw(b, n);
}

// -- the mailbox: everything not a stream message (upd_net.c) -------------

static void on_other(z_msg_t *m) {
	if (m->subject == Z_PORT_DATA_ACK) {
		if (out_on) z_port_handle_ack(&out, m);
	} else if (m->subject == Z_PORT_CLOSE) {
		if (out_on && m->from == out.peer_pid) out_on = false;
	}
	// Anything else is not for us.
}

static int io_fetch_open(void *c, const char *name, char *err, int errlen) {
	(void)c;
	return upd_net_open(name, err, errlen);
}

static int io_fetch_read(void *c, uint8_t *buf, int cap, char *err, int errlen) {
	(void)c;
	return upd_net_read(buf, cap, err, errlen);
}

static void io_fetch_close(void *c) {
	(void)c;
	upd_net_close();
}

// -- the card -----------------------------------------------------------------

static int io_size(void *c, const char *p, uint32_t *outp) {
	(void)c;
	z_fs_info_t fi;
	if (!fs_stat(p, &fi)) return UPD_ENOENT;
	if (fi.type == Z_FS_TYPE_DIR || (fi.attr & Z_FS_ATTR_DIR)) return UPD_ENOENT;
	*outp = fi.size;
	return UPD_OK;
}

static int io_open_read(void *c, const char *p) {
	uint32_t sz;
	if (io_size(c, p, &sz) != UPD_OK) return UPD_ENOENT;
	int h = fs_open_read(p);
	return h >= 0 ? h : UPD_ERR;
}

static int io_open_write(void *c, const char *p) {
	(void)c;
	int h = fs_open_write(p);
	return h >= 0 ? h : UPD_ERR;
}

static int io_read(void *c, int h, void *b, int n) {
	(void)c;
	return fs_read_chunk(h, b, n);
}

static int io_write(void *c, int h, const void *b, int n) {
	(void)c;
	return fs_write_chunk(h, b, n);
}

static int io_close(void *c, int h) {
	(void)c;
	return fs_close_handle(h) ? UPD_OK : UPD_ERR;
}

static int io_unlink(void *c, const char *p) {
	uint32_t sz;
	if (io_size(c, p, &sz) != UPD_OK) return UPD_ENOENT;
	return fs_unlink((char *)p) ? UPD_OK : UPD_ERR;
}

static int io_rename(void *c, const char *a, const char *b) {
	(void)c;
	int err = 0;
	if (fs_rename(a, b, &err)) return UPD_OK;
	switch (err) {
	case Z_FS_ERR_BUSY:  return UPD_EBUSY;
	case Z_FS_ERR_EXIST: return UPD_EEXIST;
	case Z_FS_ERR_NOENT: return UPD_ENOENT;
	default:             return UPD_ERR;
	}
}

static int io_mkdir(void *c, const char *p) {
	(void)c;
	z_fs_info_t fi;
	if (fs_stat(p, &fi))
		return (fi.type == Z_FS_TYPE_DIR || (fi.attr & Z_FS_ATTR_DIR)) ? UPD_OK : UPD_ERR;
	if (fs_mkdir(p)) return UPD_OK;
	return UPD_ERR;
}

static int io_free_kb(void *c, uint32_t *kb) {
	(void)c;
	uint32_t total = 0;
	if (!fs_df(&total, kb) || !total) return UPD_ERR;
	return UPD_OK;
}

static uint32_t io_ms(void *c) {
	(void)c;
	// Without 64-bit division, and without overflowing after hours.
	uint32_t t = z_uptime_ticks();
	return (t / Z_TICK_HZ) * 1000 + (t % Z_TICK_HZ) * 1000 / Z_TICK_HZ;
}

// -- main ---------------------------------------------------------------------

static void usage(void) {
	io_out(NULL,
		"usage: update                          what the card has\n"
		"       update RELEASE                  from " Z_DIST_NAME "\n"
		"       update latest                   whatever its LATEST names\n"
		"       update RELEASE -t http URL      from another server\n"
		"       update RELEASE -t tftp SERVER   from a TFTP server\n"
		"options: -s SETS (add sets: games,dev or all)\n"
		"         -n (what would change; changes nothing)\n"
		"         -v (hash every file, trust no manifest)\n"
		"         -f (update though the release's ABI is not the flash's)\n"
		"         -d DIR (the store is DIR/ on the server; default release)\n"
		"see docs/update.md\n");
}

// http://host[:port]/path/ -> its parts. false if it is not one.
static bool parse_url(const char *url, char *host, int hcap, uint16_t *port,
	char *path, int pcap) {
	if (strncmp(url, "http://", 7)) return false;
	const char *h = url + 7, *e = h;
	while (*e && *e != '/' && *e != ':') e++;
	if (e == h || e - h >= hcap) return false;
	memcpy(host, h, e - h);
	host[e - h] = 0;
	*port = 80;
	if (*e == ':') {
		long p = strtol(e + 1, (char **)&e, 10);
		if (p < 1 || p > 65535) return false;
		*port = (uint16_t)p;
	}
	if (*e && *e != '/') return false;
	// the store's path, without the slashes around it: "release"
	const char *p0 = *e ? e + 1 : e;
	int n = (int)strlen(p0);
	while (n && p0[n - 1] == '/') n--;
	if (!n || n >= pcap) return false;
	memcpy(path, p0, n);
	path[n] = 0;
	return true;
}

static int run(int argc, char **argv) {
	upd_opts_t o;
	memset(&o, 0, sizeof o);
	o.dir = "release";
	const char *method = NULL, *server = NULL, *url = NULL;

	for (int i = 0; i < argc; i++) {
		const char *a = argv[i];
		if (!strcmp(a, "-n")) o.dry_run = true;
		else if (!strcmp(a, "-v")) o.verify = true;
		else if (!strcmp(a, "-f")) o.force = true;
		else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return UPD_DONE; }
		else if (!strcmp(a, "-d") && i + 1 < argc) o.dir = argv[++i];
		else if (!strcmp(a, "-s") && i + 1 < argc) o.sets = argv[++i];
		else if (!strcmp(a, "-t") && i + 1 < argc) {
			method = argv[++i];
			if (!strcmp(method, "tftp")) {
				if (i + 1 >= argc) {
					io_out(NULL, "update: -t tftp needs a server: -t tftp 192.168.1.10\n");
					return UPD_USAGE;
				}
				server = argv[++i];
			} else if (!strcmp(method, "http") && i + 1 < argc &&
					!strncmp(argv[i + 1], "http://", 7)) {
				url = argv[++i];
			}
		} else if (a[0] == '-') {
			usage();
			return UPD_USAGE;
		} else if (!o.release) o.release = a;
		else { usage(); return UPD_USAGE; }
	}

	upd_io_t io = {
		NULL, io_open_read, io_open_write, io_read, io_write, io_close,
		io_size, io_unlink, io_rename, io_mkdir, io_free_kb,
		io_fetch_open, io_fetch_read, io_fetch_close, io_out, io_ms, NULL
	};

	// What the kernel and core apps in flash were built against
	// (Z_SYS_ABI); "" from a kernel older than that syscall.
	static char kabi[Z_ABI_ID_MAX];
	z_abi(kabi, sizeof kabi);
	o.kernel_abi = kabi;

	if (!o.release) {
		if (method || o.sets) { usage(); return UPD_USAGE; }
		return upd_show(&io, kabi);
	}
	if (!method) method = "http";
	if (!strcmp(method, "nfs")) {
		io_out(NULL, "update: -t takes http or tftp\n");
		return UPD_USAGE;
	}
	if (strcmp(method, "tftp") && strcmp(method, "http")) {
		io_out(NULL, "update: -t takes http or tftp\n");
		return UPD_USAGE;
	}

	char err[64], m[256];
	uint32_t server_ip = 0, net_pid = 0;
	static char host[80], path[96], src[160];
	static uint8_t keys[3 * 32];
	uint16_t port = 0;
	if (!strcmp(method, "http")) {
		// The distribution (docs/distribution.md): zeitlos.cfg's, or the
		// one this update was built with. Its key(s) likewise; a URL of
		// its own and no key means a mirror of the built-in one.
		static char cfg_url[Z_CFG_VAL_MAX], k1[Z_CFG_VAL_MAX], k2[Z_CFG_VAL_MAX];
		if (!url && z_cfg_get("system.update.url", cfg_url, sizeof cfg_url) && cfg_url[0])
			url = cfg_url;
		if (!url) url = Z_DIST_URL;
		if (!parse_url(url, host, sizeof host, &port, path, sizeof path)) {
			snprintf(m, sizeof m, "update: %s is not an http://host/path/ URL\n", url);
			io_out(NULL, m);
			return UPD_USAGE;
		}
		int nk = 0;
		bool from_cfg = false;
		if (z_cfg_get("system.update.key", k1, sizeof k1) && k1[0]) {
			from_cfg = true;
			int r = upd_keys_parse(k1, keys, 1);
			if (r < 0) {
				io_out(NULL, "update: system.update.key in " Z_PATH_SYS_CONFIG " is not "
					"a key (64 hex digits)\n");
				return UPD_USAGE;
			}
			nk += r;
			if (z_cfg_get("system.update.key2", k2, sizeof k2) && k2[0]) {
				r = upd_keys_parse(k2, keys + 32 * nk, 1);
				if (r < 0) {
					io_out(NULL, "update: system.update.key2 is not a key\n");
					return UPD_USAGE;
				}
				nk += r;
			}
		}
		if (!from_cfg) {
			nk = upd_keys_parse(Z_DIST_KEYS, keys, 3);
			if (nk < 0) nk = 0;
		}
		o.need_sig = true;
		o.keys = keys;
		o.nkeys = nk;
		o.dir = path;
		server = host;
		snprintf(src, sizeof src, "%s", url);
	} else {
		snprintf(src, sizeof src, "tftp %s", server);
	}
	o.source = src;

	if (!z_resolve_host(server, &server_ip, err, sizeof err)) {
		snprintf(m, sizeof m, "update: %s: %s\n", server, err);
		io_out(NULL, m);
		return UPD_FAILED;
	}
	if (!z_pid_lookup("net0", &net_pid) || !net_pid) {
		io_out(NULL, "update: net is not running, so there is no network\n");
		return UPD_FAILED;
	}
	if (!strcmp(method, "http"))
		upd_net_init_http(net_pid, server_ip, port, host, on_other, Z_TICK_HZ);
	else
		upd_net_init(net_pid, server_ip, on_other, Z_TICK_HZ);
	return upd_run(&io, &o);
}

int main(void) {
	static char line[Z_WM_ARG_MAX + 1], buf[2 * Z_WM_ARG_MAX + 2];
	char *argv[16];
	int argc = 0;

	if (z_launch_arg_take(line, sizeof line) && line[0])
		argc = z_args_split(line, buf, sizeof buf, argv, NULL, 16);

	// Names, before anything that waits on the mailbox: DNS reads it
	// too, and a hostname is resolved in run().
	uint32_t pid = 0;
	if (z_pid_lookup("posix0", &pid) && pid &&
			z_port_connect_arg(&out, pid, z_obj_str(PX_STDOUT_TAG)) == Z_OK)
		out_on = true;

	// One at a time: two would write the same files.
	char me[24];
	if (!z_pid_register("update", me, sizeof me) || strcmp(me, "update0")) {
		uint32_t other = 0;
		z_pid_lookup("update0", &other);
		char m[96];
		snprintf(m, sizeof m, "update: another update is running (pid %lu)\n",
			(unsigned long)other);
		io_out(NULL, m);
		if (out_on) { z_port_drain(&out, Z_TICK_HZ * 2); z_port_close(&out); }
		return UPD_FAILED;
	}

	int r = run(argc, argv);

	if (out_on) {
		z_port_drain(&out, Z_TICK_HZ * 2);
		z_port_close(&out);
	}
	return r;
}
