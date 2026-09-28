/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * fed on Linux: a zfed node as a daemon. docs/fed.md.
 *
 *   fed --dir DIR [--bind ADDR]     run: DIR/fed.cfg, DIR/node.key, DIR/store,
 *                                   and the local interface at DIR/fed.sock
 *   fed --dir DIR --print-key       the node's public key (made if missing)
 *   fed --dir DIR --join-request name=N [sysop=S] [addr=H:P] [note=T]
 *                                   a signed join request on stdout, the
 *                                   fingerprint on stderr (docs/fed.md, "Joining")
 *   fed --check-join FILE           a publisher checking one: the fingerprint,
 *                                   and the entry to add to the list
 *   fed --dir DIR --publish-list FILE   check a node list, then publish it
 *
 * and, for a network's node list without JSON by hand (core/fadmin.c):
 *
 *   sudo fed key | nodes | add NAME KEY [sysop=S] [addr=H:P] | set NAME ... | remove NAME
 *
 * --dir defaults to /var/lib/fed when it exists; run as root, fed becomes
 * the owner of that directory before touching anything in it.
 *                                   through the running fed
 *
 * One poll() loop: the TCP listener, the local socket, the sessions (one
 * outbound at a time, two inbound), and the local clients.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <stdio.h>
#include "../../../ext/monocypher/monocypher.h"
#include "../core/fadmin.h"
#include <grp.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include "../core/fnode.h"
#include "../core/fobj.h"
#include "../core/fnet.h"
#include "../../../common/zjson.h"
#include "../../../common/zplat.h"
#include "../../../ext/monocypher/monocypher-ed25519.h"

static volatile sig_atomic_t g_stop;
static void on_signal(int s) { (void)s; g_stop = 1; }

static uint32_t now_ms(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint32_t)((uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000);
}

static void nonblock(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK); }

// For the address check: a host's IPv4 address, as text.
static bool resolve4(const char *host, char *ip, int cap) {
	struct addrinfo hints, *ai = NULL;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(host, NULL, &hints, &ai) || !ai) return false;
	bool ok = inet_ntop(AF_INET, &((struct sockaddr_in *)ai->ai_addr)->sin_addr, ip, (socklen_t)cap) != NULL;
	freeaddrinfo(ai);
	return ok;
}

// -- joining (docs/fed.md, "Joining") --

// The short id in groups of four, to read aloud or compare by eye.
static void fingerprint(const uint8_t pk[32], char out[20]) {
	char sid[17];
	fobj_short_id(pk, sid);
	snprintf(out, 20, "%.4s %.4s %.4s %.4s", sid, sid + 4, sid + 8, sid + 12);
}

// A JSON string of printable text: quotes and backslashes escaped; false
// if there is a control character in it.
static bool json_str(char *out, size_t cap, const char *s) {
	size_t o = 0;
	if (cap < 3) return false;
	out[o++] = '"';
	for (; *s; s++) {
		if ((unsigned char)*s < 0x20 || *s == 0x7f) return false;
		if (o + 3 >= cap) return false;
		if (*s == '"' || *s == '\\') out[o++] = '\\';
		out[o++] = *s;
	}
	out[o++] = '"';
	out[o] = 0;
	return true;
}

static int join_request(const uint8_t seed[32], int argc, char **argv, int first) {
	const char *name = NULL, *sysop = NULL, *addr = NULL, *note = NULL;
	for (int i = first; i < argc; i++) {
		if (!strncmp(argv[i], "name=", 5)) name = argv[i] + 5;
		else if (!strncmp(argv[i], "sysop=", 6)) sysop = argv[i] + 6;
		else if (!strncmp(argv[i], "addr=", 5)) addr = argv[i] + 5;
		else if (!strncmp(argv[i], "note=", 5)) note = argv[i] + 5;
		else { fprintf(stderr, "fed: --join-request: name=, sysop=, addr=, note= (not '%s')\n", argv[i]); return 2; }
	}
	if (!name || !fnet_name_ok(name)) { fprintf(stderr, "fed: a name is 1-32 bytes of a-z 0-9 -\n"); return 2; }
	if (addr && !fnet_addr_ok(addr)) { fprintf(stderr, "fed: an addr is host:port\n"); return 2; }
	if (sysop && (!*sysop || strlen(sysop) > FNET_SYSOP_MAX)) { fprintf(stderr, "fed: a sysop is 1-%d bytes\n", FNET_SYSOP_MAX); return 2; }
	char js[1024], q[300];
	int o = snprintf(js, sizeof(js), "{\"name\":\"%s\"", name);
	if (sysop) { if (!json_str(q, sizeof(q), sysop)) { fprintf(stderr, "fed: a control character in sysop\n"); return 2; } o += snprintf(js + o, sizeof(js) - (size_t)o, ",\"sysop\":%s", q); }
	if (addr) o += snprintf(js + o, sizeof(js) - (size_t)o, ",\"addr\":\"%s\"", addr);
	if (note) { if (!json_str(q, sizeof(q), note)) { fprintf(stderr, "fed: a control character (or too long) in note\n"); return 2; } o += snprintf(js + o, sizeof(js) - (size_t)o, ",\"note\":%s", q); }
	snprintf(js + o, sizeof(js) - (size_t)o, "}");
	uint8_t sk[64], pk[32], s2[32], obj[FOBJ_MAX];
	memcpy(s2, seed, 32);
	crypto_ed25519_key_pair(sk, pk, s2);
	fobj_t ob;
	memset(&ob, 0, sizeof(ob));
	snprintf(ob.topic, sizeof(ob.topic), "fed/join");
	snprintf(ob.type, sizeof(ob.type), "fed.join");
	ob.format = FOBJ_JSON; ob.kind = FOBJ_LOG; ob.time = (uint64_t)time(NULL); ob.seq = 1; ob.len = (uint32_t)strlen(js);
	int n = fobj_make(&ob, (const uint8_t *)js, sk, obj, sizeof(obj));
	crypto_wipe(sk, sizeof(sk));
	if (n < 0) { fprintf(stderr, "fed: %s\n", fobj_strerror(n)); return 1; }
	fwrite(obj, 1, (size_t)n, stdout);
	char fp[20];
	fingerprint(pk, fp);
	fprintf(stderr, "fingerprint: %s\n", fp);
	return 0;
}

static int read_file(const char *path, uint8_t *buf, int cap) {
	FILE *f = fopen(path, "rb");
	if (!f) return -1;
	int n = (int)fread(buf, 1, (size_t)cap, f);
	fclose(f);
	return n;
}

static int check_join(const char *path) {
	static uint8_t buf[FOBJ_MAX + 1];
	static zjson_tok_t tok[64];
	int n = read_file(path, buf, sizeof(buf));
	fobj_t o;
	if (n <= 0 || fobj_parse(buf, (uint32_t)n, 0, &o, NULL) || o.size != (uint32_t)n) { fprintf(stderr, "fed: not a zfed object\n"); return 1; }
	if (fobj_verify(&o)) { fprintf(stderr, "fed: the signature does not check -- not from the key it names\n"); return 1; }
	if (strcmp(o.topic, "fed/join") || strcmp(o.type, "fed.join") || o.format != FOBJ_JSON) { fprintf(stderr, "fed: not a join request\n"); return 1; }
	const char *js = (const char *)o.payload;
	if (zjson_parse(js, o.len, tok, 64, NULL) < 1 || tok[0].type != ZJ_OBJECT) { fprintf(stderr, "fed: its payload is not strict JSON\n"); return 1; }
	char name[40] = "", sysop[80] = "", addr[110] = "", entry[600], q[200], key[65];
	int t;
	if ((t = zjson_get(js, tok, 0, "name")) < 0 || !zjson_str(js, &tok[t], name, sizeof(name)) || !fnet_name_ok(name)) { fprintf(stderr, "fed: no good name\n"); return 1; }
	if ((t = zjson_get(js, tok, 0, "sysop")) >= 0 && (!zjson_str(js, &tok[t], sysop, sizeof(sysop)) || !sysop[0] || strlen(sysop) > FNET_SYSOP_MAX)) { fprintf(stderr, "fed: a bad sysop\n"); return 1; }
	if ((t = zjson_get(js, tok, 0, "addr")) >= 0 && (!zjson_str(js, &tok[t], addr, sizeof(addr)) || !fnet_addr_ok(addr))) { fprintf(stderr, "fed: a bad addr\n"); return 1; }
	fobj_hex(o.origin, 32, key);
	int e = snprintf(entry, sizeof(entry), "{\"key\":\"%s\",\"name\":\"%s\"", key, name);
	if (sysop[0]) { if (!json_str(q, sizeof(q), sysop)) { fprintf(stderr, "fed: a control character in sysop\n"); return 1; } e += snprintf(entry + e, sizeof(entry) - (size_t)e, ",\"sysop\":%s", q); }
	if (addr[0]) e += snprintf(entry + e, sizeof(entry) - (size_t)e, ",\"addr\":\"%s\"", addr);
	snprintf(entry + e, sizeof(entry) - (size_t)e, "}");
	char fp[20];
	fingerprint(o.origin, fp);
	printf("fingerprint: %s  (compare it with theirs)\n", fp);
	if ((t = zjson_get(js, tok, 0, "note")) >= 0 && zjson_str(js, &tok[t], q, sizeof(q))) printf("note: %s\n", q);
	printf("entry: %s\n", entry);
	return 0;
}

static int publish_list(const char *dir, const char *path) {
	static uint8_t js[FOBJ_PAYLOAD_MAX + 1];
	static zjson_tok_t all[2048];
	static fnet_list_t list;
	char net[40], err[160];
	int n = read_file(path, js, FOBJ_PAYLOAD_MAX + 1);
	if (n <= 0 || n > FOBJ_PAYLOAD_MAX) { fprintf(stderr, "fed: cannot read %s, or over 16 KB\n", path); return 1; }
	// the network it names, then the whole list checked as members will check it
	int t;
	if (zjson_parse((const char *)js, (uint32_t)n, all, 2048, NULL) < 1 || (t = zjson_get((const char *)js, all, 0, "network")) < 0 ||
			!zjson_str((const char *)js, &all[t], net, sizeof(net)) || !fnet_name_ok(net)) {
		fprintf(stderr, "fed: no good \"network\" in it\n");
		return 1;
	}
	if (fnet_parse(net, js, (uint32_t)n, &list, err, sizeof(err))) { fprintf(stderr, "fed: the list would be refused: %s\n", err); return 1; }
	struct sockaddr_un ua;
	memset(&ua, 0, sizeof(ua));
	ua.sun_family = AF_UNIX;
	snprintf(ua.sun_path, sizeof(ua.sun_path), "%s/fed.sock", dir);
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *)&ua, sizeof(ua))) { fprintf(stderr, "fed: is fed running? (%s)\n", ua.sun_path); return 1; }
	char hdr[160];
	int h = snprintf(hdr, sizeof(hdr), "PUB %s/nodes fed.nodes json state list %d\n", net, n);
	if (write(fd, hdr, (size_t)h) != h || write(fd, js, (size_t)n) != n) { fprintf(stderr, "fed: write failed\n"); return 1; }
	char reply[200];
	int r = (int)read(fd, reply, sizeof(reply) - 1);
	close(fd);
	if (r <= 0) { fprintf(stderr, "fed: no reply\n"); return 1; }
	reply[r] = 0;
	printf("%s: %d nodes: %s", net, list.n, reply);
	return strncmp(reply, "OK", 2) ? 1 : 0;
}

// -- `fed key`, `fed nodes`, `fed add`...: fadmin.c, over the socket --

static const char *g_dir;

static int lx_request(const char *req, const uint8_t *payload, int plen, char *line, int lcap, uint8_t *data, int dcap, void *ctx) {
	(void)ctx;
	struct sockaddr_un ua;
	memset(&ua, 0, sizeof(ua));
	ua.sun_family = AF_UNIX;
	snprintf(ua.sun_path, sizeof(ua.sun_path), "%s/fed.sock", g_dir);
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *)&ua, sizeof(ua))) { if (fd >= 0) close(fd); return -1; }
	if (write(fd, req, strlen(req)) < 0 || (plen && write(fd, payload, (size_t)plen) != plen)) { close(fd); return -1; }
	int ll = 0;
	char ch;
	while (ll < lcap - 1 && read(fd, &ch, 1) == 1 && ch != '\n') line[ll++] = ch;
	line[ll] = 0;
	int n = 0;
	if (data && !strncmp(line, "OK ", 3)) {
		int want = atoi(line + 3);
		if (want > dcap) want = dcap;
		while (n < want) { ssize_t r = read(fd, data + n, (size_t)(want - n)); if (r <= 0) break; n += (int)r; }
	}
	close(fd);
	return n;
}

// The PUBLIC key, from node.key -- never made here: that is fed's first run.
static bool lx_public_key(uint8_t pk[32], void *ctx) {
	(void)ctx;
	char p[300];
	uint8_t seed[32], sk[64];
	snprintf(p, sizeof(p), "%s/node.key", g_dir);
	FILE *f = fopen(p, "rb");
	if (!f) return false;
	bool ok = fread(seed, 1, 32, f) == 32;
	fclose(f);
	if (ok) crypto_ed25519_key_pair(sk, pk, seed);
	crypto_wipe(seed, sizeof(seed));
	crypto_wipe(sk, sizeof(sk));
	return ok;
}

static bool lx_read_cfg(char *text, int cap, void *ctx) {
	(void)ctx;
	char p[300];
	snprintf(p, sizeof(p), "%s/fed.cfg", g_dir);
	FILE *f = fopen(p, "r");
	if (!f) return false;
	size_t n = fread(text, 1, (size_t)cap - 1, f);
	fclose(f);
	text[n] = 0;
	return true;
}

static void lx_out(const char *line, void *ctx) { (void)ctx; puts(line); }

// Run as root: become the data directory's owner first (bbs, on a
// server), so that nothing in it is ever made or owned by root.
static bool drop_to_owner(const char *dir) {
	struct stat st;
	if (geteuid() != 0 || stat(dir, &st) || st.st_uid == 0) return true;
	if (setgroups(0, NULL) || setgid(st.st_gid) || setuid(st.st_uid)) {
		fprintf(stderr, "fed: cannot become the owner of %s\n", dir);
		return false;
	}
	return true;
}

// The node key: DIR/node.key, 32 random bytes, 0600 -- made if missing.
static bool node_seed(const char *dir, uint8_t seed[32]) {
	char p[300];
	snprintf(p, sizeof(p), "%s/node.key", dir);
	FILE *f = fopen(p, "rb");
	if (f) { bool ok = fread(seed, 1, 32, f) == 32; fclose(f); return ok; }
	plat_random(seed, 32);
	int fd = open(p, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (fd < 0) return false;
	bool ok = write(fd, seed, 32) == 32 && fsync(fd) == 0;
	close(fd);
	return ok;
}

typedef struct { int fd; fsess_t *s; bool connecting; } conn_t;
static conn_t g_conn[FNODE_SESSIONS];
static int g_cfd[FNODE_CLIENTS];

static void conn_end(conn_t *c) {
	if (c->fd >= 0) close(c->fd);
	fnode_session_end(c->s, now_ms());
	c->fd = -1;
	c->s = NULL;
}

// Starts connecting to peer i: non-blocking; finished in the loop.
static void dial(int i) {
	const fcfg_t *cfg = fnode_cfg();
	const fpeer_t *p = &cfg->peers[i];
	conn_t *c = NULL;
	for (int k = 0; k < FNODE_SESSIONS; k++) if (!g_conn[k].s) { c = &g_conn[k]; break; }
	if (!c) return;
	fsess_t *s = fnode_session(true, i, p->host, now_ms());
	if (!s) return;
	c->s = s;
	c->fd = -1;
	char port[8];
	snprintf(port, sizeof(port), "%u", p->port);
	struct addrinfo hints, *ai = NULL;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(p->host, port, &hints, &ai) || !ai) { conn_end(c); return; }
	c->fd = socket(ai->ai_family, SOCK_STREAM, 0);
	if (c->fd < 0) { freeaddrinfo(ai); conn_end(c); return; }
	nonblock(c->fd);
	int r = connect(c->fd, ai->ai_addr, ai->ai_addrlen);
	freeaddrinfo(ai);
	if (r < 0 && errno != EINPROGRESS) { conn_end(c); return; }
	c->connecting = true;
}

int main(int argc, char **argv) {
	const char *dir = NULL, *bind_addr = "0.0.0.0", *check = NULL, *list_file = NULL, *network = NULL;
	bool print_key = false;
	int join_at = 0, cmd_at = 0;
	for (int i = 1; i < argc; i++) {
		if (fadmin_is_command(argv[i])) { cmd_at = i; break; }
		if (!strcmp(argv[i], "--dir") && i + 1 < argc) dir = argv[++i];
		else if (!strcmp(argv[i], "--network") && i + 1 < argc) network = argv[++i];
		else if (!strcmp(argv[i], "--bind") && i + 1 < argc) bind_addr = argv[++i];
		else if (!strcmp(argv[i], "--print-key")) print_key = true;
		else if (!strcmp(argv[i], "--check-join") && i + 1 < argc) check = argv[++i];
		else if (!strcmp(argv[i], "--publish-list") && i + 1 < argc) list_file = argv[++i];
		else if (!strcmp(argv[i], "--join-request")) { join_at = i + 1; break; }
		else { fprintf(stderr, "usage: fed [--dir DIR] [--bind ADDR]                 run the node\n"
			"       fed [--dir DIR] key | nodes                     this node's public key; the network's list\n"
			"       fed [--dir DIR] add NAME KEY [sysop=S] [addr=HOST:PORT]\n"
			"       fed [--dir DIR] set NAME [sysop=S] [addr=HOST:PORT] | remove NAME\n"
			"       fed --dir DIR --join-request name=... | --check-join FILE | --publish-list FILE\n"
			"  (--dir: /var/lib/fed when it exists; --network NAME where fed.cfg names several)\n"); return 2; }
	}
	if (check) return check_join(check);
	if (!dir) {
		struct stat st;
		if (!stat("/var/lib/fed", &st) && S_ISDIR(st.st_mode)) dir = "/var/lib/fed";
		else { fprintf(stderr, "fed: no /var/lib/fed -- say where: --dir DIR\n"); return 2; }
	}
	if (!drop_to_owner(dir)) return 1;
	if (cmd_at) {
		fadmin_io_t io = { lx_request, lx_public_key, lx_read_cfg, lx_out, NULL, "sudo systemctl start fed", NULL };
		static char where[320];
		snprintf(where, sizeof(where), "%s/node.key", dir);
		io.private_key_where = where;
		g_dir = dir;
		return fadmin(&io, network, argc - cmd_at, argv + cmd_at);
	}
	if (list_file) return publish_list(dir, list_file);
	mkdir(dir, 0700);
	uint8_t seed[32];
	if (!node_seed(dir, seed)) { fprintf(stderr, "fed: cannot read or make %s/node.key\n", dir); return 1; }
	if (join_at) return join_request(seed, argc, argv, join_at);
	if (print_key) {
		uint8_t sk[64], pk[32];
		char h[65];
		crypto_ed25519_key_pair(sk, pk, seed);
		fobj_hex(pk, 32, h);
		printf("%s\n", h);
		return 0;
	}
	char err[160];
	fnode_set_resolver(resolve4);
	if (fnode_start(dir, seed, err, sizeof(err))) { fprintf(stderr, "fed: %s\n", err); return 1; }
	memset(seed, 0, sizeof(seed));
	const fcfg_t *cfg = fnode_cfg();
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGPIPE, SIG_IGN);

	int lfd = -1;
	if (cfg->listen) {
		struct sockaddr_in a;
		memset(&a, 0, sizeof(a));
		a.sin_family = AF_INET;
		a.sin_port = htons(cfg->listen);
		if (inet_pton(AF_INET, bind_addr, &a.sin_addr) != 1) { fprintf(stderr, "fed: bad --bind\n"); return 1; }
		lfd = socket(AF_INET, SOCK_STREAM, 0);
		int one = 1;
		setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		if (bind(lfd, (struct sockaddr *)&a, sizeof(a)) || listen(lfd, 4)) { perror("fed: listen"); return 1; }
		nonblock(lfd);
	}
	struct sockaddr_un ua;
	memset(&ua, 0, sizeof(ua));
	ua.sun_family = AF_UNIX;
	snprintf(ua.sun_path, sizeof(ua.sun_path), "%s/fed.sock", dir);
	unlink(ua.sun_path);
	int ufd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (bind(ufd, (struct sockaddr *)&ua, sizeof(ua)) || listen(ufd, 4)) { perror("fed: local socket"); return 1; }
	chmod(ua.sun_path, 0600);
	nonblock(ufd);
	{
		char h[65], line[200];
		fobj_hex(fnode_public_key(), 32, h);
		snprintf(line, sizeof(line), "fed: %s up, key %s, %s", cfg->name[0] ? cfg->name : "node", h,
			cfg->listen ? "listening" : "outbound only");
		plat_log(line);
	}
	for (int i = 0; i < FNODE_SESSIONS; i++) { g_conn[i].fd = -1; g_conn[i].s = NULL; }
	for (int i = 0; i < FNODE_CLIENTS; i++) g_cfd[i] = -1;

	while (!g_stop) {
		uint32_t t = now_ms();
		int due = fnode_due(t);
		if (due >= 0) dial(due);

		struct pollfd pf[2 + FNODE_SESSIONS + FNODE_CLIENTS];
		int np = 0, li = -1, ui, si[FNODE_SESSIONS], ci[FNODE_CLIENTS];
		if (lfd >= 0) { li = np; pf[np++] = (struct pollfd){ lfd, POLLIN, 0 }; }
		ui = np; pf[np++] = (struct pollfd){ ufd, POLLIN, 0 };
		for (int i = 0; i < FNODE_SESSIONS; i++) {
			si[i] = -1;
			conn_t *c = &g_conn[i];
			if (!c->s || c->fd < 0) continue;
			const uint8_t *p;
			short ev = POLLIN;
			if (c->connecting || fsess_output(c->s, &p)) ev |= POLLOUT;
			si[i] = np; pf[np++] = (struct pollfd){ c->fd, ev, 0 };
		}
		for (int i = 0; i < FNODE_CLIENTS; i++) {
			ci[i] = -1;
			if (g_cfd[i] < 0) continue;
			const uint8_t *p;
			short ev = POLLIN;
			if (fnode_client_output(i, &p)) ev |= POLLOUT;
			ci[i] = np; pf[np++] = (struct pollfd){ g_cfd[i], ev, 0 };
		}
		poll(pf, (nfds_t)np, 200);
		t = now_ms();

		if (li >= 0 && (pf[li].revents & POLLIN)) {
			struct sockaddr_storage sa;
			socklen_t sl = sizeof(sa);
			int fd = accept(lfd, (struct sockaddr *)&sa, &sl);
			if (fd >= 0) {
				char addr[64] = "?";
				if (sa.ss_family == AF_INET) inet_ntop(AF_INET, &((struct sockaddr_in *)&sa)->sin_addr, addr, sizeof(addr));
				else if (sa.ss_family == AF_INET6) inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&sa)->sin6_addr, addr, sizeof(addr));
				conn_t *c = NULL;
				for (int k = 0; k < FNODE_SESSIONS; k++) if (!g_conn[k].s) { c = &g_conn[k]; break; }
				fsess_t *s = (c && fnode_admit(addr, t)) ? fnode_session(false, -1, addr, t) : NULL;
				if (!s) close(fd);				// busy, or waiting out a limit: nothing read, nothing said
				else { nonblock(fd); c->fd = fd; c->s = s; c->connecting = false; }
			}
		}
		if (pf[ui].revents & POLLIN) {
			int fd = accept(ufd, NULL, NULL);
			if (fd >= 0) {
				int id = fnode_client_open();
				if (id < 0) close(fd);
				else { nonblock(fd); g_cfd[id] = fd; }
			}
		}
		for (int i = 0; i < FNODE_SESSIONS; i++) {
			conn_t *c = &g_conn[i];
			if (!c->s) continue;
			if (c->fd < 0) { conn_end(c); continue; }
			short re = si[i] >= 0 ? pf[si[i]].revents : 0;
			if (c->connecting) {
				if (!(re & (POLLOUT | POLLERR | POLLHUP))) { fsess_poll(c->s, t); if (c->s->state != FS_RUNNING) conn_end(c); continue; }
				int e = 0; socklen_t el = sizeof(e);
				getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &e, &el);
				if (e) { conn_end(c); continue; }
				c->connecting = false;
			}
			if (re & POLLIN) {
				uint8_t b[4096];
				ssize_t r = read(c->fd, b, sizeof(b));
				if (r > 0) fsess_input(c->s, b, (uint32_t)r);
				else if (r == 0 || (errno != EAGAIN && errno != EINTR)) fsess_closed(c->s);
			}
			const uint8_t *p;
			uint32_t n;
			while (c->s->state != FS_FAILED && (n = fsess_output(c->s, &p)) > 0) {
				ssize_t w = write(c->fd, p, n);
				if (w <= 0) break;
				fsess_consumed(c->s, (uint32_t)w);
			}
			fsess_poll(c->s, t);
			if (c->s->state != FS_RUNNING) conn_end(c);
		}
		for (int i = 0; i < FNODE_CLIENTS; i++) {
			if (g_cfd[i] < 0) continue;
			short re = ci[i] >= 0 ? pf[ci[i]].revents : 0;
			if (re & POLLIN) {
				uint8_t b[4096];
				ssize_t r = read(g_cfd[i], b, sizeof(b));
				if (r > 0) fnode_client_input(i, b, (uint32_t)r);
				else if (r == 0 || (errno != EAGAIN && errno != EINTR)) { close(g_cfd[i]); g_cfd[i] = -1; fnode_client_close(i); continue; }
			}
			const uint8_t *p;
			uint32_t n;
			while ((n = fnode_client_output(i, &p)) > 0) {
				ssize_t w = write(g_cfd[i], p, n);
				if (w <= 0) break;
				fnode_client_consumed(i, (uint32_t)w);
			}
		}
		fnode_tick(t);
	}
	// Before stopping: whatever clients have already sent -- an ACK in
	// flight above all. Delivery is at least once (an object counts as
	// delivered when its ACK is processed), so an unread ACK means the
	// object comes again after a restart; a clean stop should not cause
	// that. (A crash still can: consumers ignore repeats by object id.)
	for (int i = 0; i < FNODE_CLIENTS; i++) {
		if (g_cfd[i] < 0) continue;
		uint8_t b[4096];
		ssize_t r;
		while ((r = read(g_cfd[i], b, sizeof(b))) > 0) fnode_client_input(i, b, (uint32_t)r);
		close(g_cfd[i]);
		g_cfd[i] = -1;
		fnode_client_close(i);
	}
	for (int i = 0; i < FNODE_SESSIONS; i++) if (g_conn[i].s) conn_end(&g_conn[i]);
	fnode_stop();
	unlink(ua.sun_path);
	plat_log("fed: stopped");
	return 0;
}
