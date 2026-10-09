/*
 * Host test for sw/apps/zlink: two copies of the REAL zlinkapp.c, in two
 * processes, linked by simulated wires.
 *
 *   make -C sw/apps/zlink/tests
 *
 * Each process is one machine: its service (serve()) runs as it does
 * on the target, and this file plays everything around it -- the
 * kernel's messages, a `zlink` command line, term, posix, the
 * filesystem (a directory each), the password, GPIO and a stream
 * engine. The two share one block of memory: the two wires, and the
 * two engines' FIFOs.
 *
 * The wires are modelled as electricity, not as messages: each machine
 * can pull either wire low (soft zlink), and an engine DRIVES its TX
 * wire. A wire an engine drives reads as the engine's bits to anyone
 * else -- noise to a soft receiver. Two drivers on one wire, or soft
 * pulling a wire an engine drives, is a VIOLATION -- the thing the
 * upgrade's timing exists to prevent -- and fails the test.
 *
 * A stream engine moves symbols straight into the other machine's RX
 * FIFO, but only if that machine's engine is on, receiving, on the
 * wire this one drives, at the same rate -- and, to test stepping down,
 * only up to a rate the scenario sets.
 *
 * Scenarios:
 *   1. soft only (B has no engine): ls, get, put both ways, refusals,
 *      speed, a shell with a wrong and a right password
 *   2. both have engines, crossed cable: the upgrade to 12 Mbit/s, big
 *      files both ways, then one machine stops and the other goes back
 *      to soft
 *   3. stream works only up to 3 Mbit/s: the ladder steps down to it
 *   4. -m stream on fixed pins, no soft at all
 *   5. the secondary's answer to the first offer is lost: it goes onto
 *      its engine regardless, the primary goes quiet, neither drives a
 *      wire the other pulls, and the next try works
 *
 * Exit 0 on success.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/stat.h>

#define main zlink_main_unused
#include "../zlinkapp.c"
#undef main

// =========================================================================
// What the two machines share
// =========================================================================

typedef struct {
	uint32_t sctl, spins, div, lock;
	uint16_t rx[1024];
	int rxh, rxn;
	bool rxovr;
} eng_t;

typedef struct {
	volatile int lk;
	bool pull[2][2];			// [machine][wire]: soft holding it low
	eng_t eng[2];
	int crossed;				// the cable: A's X is B's Y
	int engines[2];				// engines each machine's bitstream has
	uint32_t max_kbit;			// a stream faster than this loses everything
	uint32_t violations;
	char violation[200];
	volatile int flag[2];		// the scripts' handshakes
	volatile int lose;			// upgrade answers still to lose, either machine
	uint32_t noise;
} shm_t;

static shm_t *sh;
volatile int *zl_test_lose_p;
static int me;					// 0 = A, 1 = B
static char root[2][256];		// each machine's filesystem
static bool has_pw[2];
static bool has_posix[2];

static void lock(void) { while (__sync_lock_test_and_set(&sh->lk, 1)) ; }
static void unlock(void) { __sync_lock_release(&sh->lk); }

static double real_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void fail(const char *fmt, const char *a) {
	fprintf(stderr, "[%c] FAIL: ", 'A' + me);
	fprintf(stderr, fmt, a ? a : "");
	fprintf(stderr, "\n");
	exit(1);
}

// -- wires --------------------------------------------------------------------

// The wire a machine's flat pin number is on, or -1. Port 0 pins 0
// and 1 are X and Y.
static int wire_of(int m, int flat) {
	if (flat != 0 && flat != 1) return -1;
	return (m == 1 && sh->crossed) ? 1 - flat : flat;
}

static bool eng_on(int m) { return (sh->eng[m].sctl & 7u) == Z_GS_ZLINK; }

static int eng_role_wire(int m, int role) {
	uint32_t b = (sh->eng[m].spins >> (8 * role)) & 0xffu;
	if (!eng_on(m) || !(b & 0x80u)) return -1;
	return wire_of(m, (int)(b & 0x3fu));
}

static bool eng_owns(int m, int flat) {
	if ((sh->eng[m].sctl & 7u) == 0) return false;
	for (int r = 0; r < 4; r++) {
		uint32_t b = (sh->eng[m].spins >> (8 * r)) & 0xffu;
		if ((b & 0x80u) && (int)(b & 0x3fu) == flat) return true;
	}
	return false;
}

// Called with the lock held, after anything that changes who drives.
static void check(void) {
	for (int w = 0; w < 2; w++) {
		int drivers = 0, drv = -1;
		for (int m = 0; m < 2; m++)
			if (eng_role_wire(m, 0) == w) { drivers++; drv = m; }
		if (drivers == 2) {
			sh->violations++;
			snprintf(sh->violation, sizeof sh->violation, "both engines drive wire %d", w);
		} else if (drivers == 1 && sh->pull[1 - drv][w]) {
			sh->violations++;
			snprintf(sh->violation, sizeof sh->violation,
			         "%c pulls wire %d low while %c's engine drives it", 'A' + 1 - drv, w, 'A' + drv);
		}
	}
}

bool z_gpio_present(void) { return true; }
uint32_t z_gpio_port_count(void) { return 2; }

void z_gpio_mode(uint32_t port, uint32_t pin, z_gpio_mode_t mode) {
	int w = port ? -1 : wire_of(me, (int)pin);
	if (w < 0 || mode != Z_GPIO_OD) return;
	lock();
	if (!eng_owns(me, (int)pin)) sh->pull[me][w] = false;
	unlock();
}

void z_gpio_od_write(uint32_t port, uint32_t pin, bool value) {
	int w = port ? -1 : wire_of(me, (int)pin);
	if (w < 0) return;
	lock();
	// an engine owns its pins: GPIO's writes do not reach them
	if (!eng_owns(me, (int)pin)) {
		sh->pull[me][w] = !value;
		check();
	}
	unlock();
}

bool z_gpio_read(uint32_t port, uint32_t pin) {
	int w = port ? -1 : wire_of(me, (int)pin);
	bool v;
	if (w < 0) return true;
	lock();
	if (eng_role_wire(0, 0) == w || eng_role_wire(1, 0) == w) v = (++sh->noise >> 2) & 1;
	else v = !(sh->pull[0][w] || sh->pull[1][w]);
	unlock();
	return v;
}

// -- the engine, behind zgpio_stream.c's register hooks --------------------------

static void rx_push(eng_t *g, uint16_t v) {
	if (g->rxn == 1024) { g->rxovr = true; return; }
	g->rx[(g->rxh + g->rxn) % 1024] = v;
	g->rxn++;
}

static uint16_t rx_pop(eng_t *g) {
	uint16_t v = g->rx[g->rxh];
	g->rxh = (g->rxh + 1) % 1024;
	g->rxn--;
	return v;
}

// One symbol onto the wire. Called with the lock held.
static void tx(uint16_t v) {
	eng_t *g = &sh->eng[me], *p = &sh->eng[1 - me];
	int w = eng_role_wire(me, 0);
	if (w < 0) return;
	if (!eng_on(1 - me) || !(p->sctl & Z_GS_RXEN) || eng_role_wire(1 - me, 1) != w) return;
	if (p->div != g->div || 12000u / (g->div + 1) > sh->max_kbit) return;
	rx_push(p, v);
}

uint32_t z_gs_test_rd(uint32_t a) {
	uint32_t v = 0;
	if (a == 0xe0000008u) return 0x5A475049u;
	if (a == 0xe000000cu)
		return 0x47500000u | (sh->engines[me] ? 0xFu << 8 : 0) | ((uint32_t)sh->engines[me] << 4) | 2u;
	if ((a - Z_GS_BASE) / Z_GS_SIZE != 0) return 0;
	eng_t *g = &sh->eng[me];
	lock();
	switch ((a - Z_GS_BASE) % Z_GS_SIZE) {
	case Z_GS_REG_SCTL: v = g->sctl; break;
	case Z_GS_REG_SPINS: v = g->spins; break;
	case Z_GS_REG_SRATE: v = g->div; break;
	case Z_GS_REG_SSTAT:
		v = ((uint32_t)g->rxn << 16) | (g->rxovr ? Z_GS_ST_RXOVR : 0) | Z_GS_ST_ALIGNED;
		break;
	case Z_GS_REG_SRX:
		v = g->rxn ? 0x200u | rx_pop(g) : 0;
		break;
	case Z_GS_REG_SRX4:
		if (g->rxn >= 4) for (int i = 0; i < 4; i++) v |= (uint32_t)(rx_pop(g) & 0xff) << (8 * i);
		break;
	case Z_GS_REG_SLOCK: v = g->lock; g->lock = 1; break;
	}
	unlock();
	return v;
}

void z_gs_test_wr(uint32_t a, uint32_t v) {
	if ((a - Z_GS_BASE) / Z_GS_SIZE != 0) return;
	eng_t *g = &sh->eng[me];
	lock();
	switch ((a - Z_GS_BASE) % Z_GS_SIZE) {
	case Z_GS_REG_SCTL:
		if ((v & 7u) != (g->sctl & 7u)) { g->rxn = 0; g->rxovr = false; }
		g->sctl = v;
		// the pins it takes are no longer GPIO's: whatever soft was
		// doing on them stops
		for (int pin = 0; pin < 2; pin++)
			if (eng_owns(me, pin)) sh->pull[me][wire_of(me, pin)] = false;
		check();
		break;
	case Z_GS_REG_SPINS: g->spins = v; break;
	case Z_GS_REG_SRATE: g->div = v; break;
	case Z_GS_REG_STX: tx((uint16_t)(v & 0x1ff)); break;
	case Z_GS_REG_STX4: for (int i = 0; i < 4; i++) tx((uint16_t)((v >> (8 * i)) & 0xff)); break;
	case Z_GS_REG_SFLUSH:
		if (v & 2) g->rxn = 0;
		if (v & 4) g->rxovr = false;
		break;
	case Z_GS_REG_SLOCK: g->lock = v & 1; break;
	}
	unlock();
}

// =========================================================================
// The kernel, and the processes around the service
// =========================================================================

#define SVC   1
#define CLI   50
#define TERM  60
#define POSIX 70

uint32_t z_uptime_ticks(void) {
	static double t0;
	if (!t0) t0 = real_ms();
	return (uint32_t)((real_ms() - t0) * Z_TICK_HZ / 1000.0);
}

static void step(void);

// -- messages to the service ----------------------------------------------------

typedef struct { z_msg_t m; z_blob_t blob; uint8_t data[4200]; char str[300]; } qent_t;
static qent_t q[512];
static int qh, qn;

static void to_svc(uint32_t from, uint32_t subj, uint32_t tag, z_obj_t obj) {
	if (qn == 512) fail("the service's mailbox is full%s", NULL);
	qent_t *e = &q[(qh + qn++) % 512];
	memset(&e->m, 0, sizeof e->m);
	e->m.from = from;
	e->m.to = SVC;
	e->m.subject = subj;
	e->m.tag = tag;
	e->m.obj = obj;
	if (obj.type == Z_BLOB) {
		const z_blob_t *b = obj.val.ptr;
		e->blob.len = b->len;
		memcpy(e->data, b->data, b->len);
		e->blob.data = e->data;
		e->m.obj.val.ptr = &e->blob;
	} else if (obj.type == Z_STR) {
		snprintf(e->str, sizeof e->str, "%s", obj.val.str);
		e->m.obj.val.str = e->str;
	}
}

static z_obj_t blob_of(z_blob_t *b, const void *d, uint32_t n) {
	z_obj_t o;
	b->len = n;
	b->data = (uint8_t *)d;
	o.type = Z_BLOB;
	o.val.ptr = b;
	return o;
}

z_rv z_msg_read(z_msg_t *m) {
	if (!qn) step();
	if (!qn) return Z_FAIL;
	*m = q[qh].m;
	qh = (qh + 1) % 512;
	qn--;
	return Z_OK;
}

void z_proc_wait(uint32_t t) {
	(void)t;
	usleep(200);
	step();
}

z_obj_t z_obj_none(void) { z_obj_t o; memset(&o, 0, sizeof o); o.type = Z_NONE; return o; }
z_obj_t z_obj_uint32(uint32_t u) { z_obj_t o = z_obj_none(); o.type = Z_UINT32; o.val.uint32 = u; return o; }
z_obj_t z_obj_str(const char *s) { z_obj_t o = z_obj_none(); o.type = Z_STR; o.val.str = (char *)s; return o; }
uint32_t z_blob_len(const z_obj_t *o) { return o->type == Z_BLOB ? ((z_blob_t *)o->val.ptr)->len : 0; }
void *z_blob_data(const z_obj_t *o) { return o->type == Z_BLOB ? ((z_blob_t *)o->val.ptr)->data : NULL; }

// -- the actors ------------------------------------------------------------------

static struct {
	bool active, connected, closed, refused, marker;
	char out[16384];
	int n, rc;
} cli_a;

static struct {
	bool connected, closed, refused;
	uint32_t conn;
	char out[16384];
	int n;
} term;

static struct {
	bool connected, closed, map_ok, replied;
	char in[2048];
	int n;
} px;

static void from_svc(uint32_t to, uint32_t subj, uint32_t tag, z_obj_t obj) {
	uint32_t n = z_blob_len(&obj);
	const uint8_t *d = z_blob_data(&obj);
	if (to == CLI) {
		if (subj == Z_PORT_CONNECTED) cli_a.connected = true;
		else if (subj == Z_PORT_REFUSED) cli_a.refused = cli_a.closed = true;
		else if (subj == Z_PORT_CLOSE) cli_a.closed = true;
		else if (subj == Z_PORT_DATA) {
			for (uint32_t i = 0; i < n; i++) {
				if (cli_a.marker) { cli_a.rc = d[i] - '0'; cli_a.marker = false; }
				else if (d[i] == 1) cli_a.marker = true;
				else if (cli_a.n < (int)sizeof cli_a.out - 1) cli_a.out[cli_a.n++] = (char)d[i];
			}
			cli_a.out[cli_a.n] = 0;
			to_svc(CLI, Z_PORT_DATA_ACK, tag, z_obj_none());
		}
	} else if (to == TERM) {
		if (subj == Z_PORT_CONNECTED) { term.connected = true; term.conn = obj.val.uint32; }
		else if (subj == Z_PORT_REFUSED) term.refused = true;
		else if (subj == Z_PORT_CLOSE) term.closed = true;
		else if (subj == Z_PORT_DATA) {
			for (uint32_t i = 0; i < n && term.n < (int)sizeof term.out - 1; i++)
				term.out[term.n++] = (char)d[i];
			term.out[term.n] = 0;
			to_svc(TERM, Z_PORT_DATA_ACK, tag, z_obj_none());
		}
	} else if (to == POSIX) {
		if (subj == Z_PORT_CONNECT) {
			// the identity map: transport zlink, auth system
			if (obj.type == Z_MAP) {
				z_obj_table_t *t = obj.val.ptr;
				bool tr = false, au = false;
				for (uint32_t i = 0; i < t->len; i++) {
					if (!strcmp(t->a[i].val.str, Z_PORT_ID_TRANSPORT) && !strcmp(t->b[i].val.str, "zlink")) tr = true;
					if (!strcmp(t->a[i].val.str, Z_PORT_ID_AUTH) &&
					    !strcmp(t->b[i].val.str, Z_PORT_AUTH_SYSTEM)) au = true;
				}
				px.map_ok = tr && au;
			}
			px.connected = true;
			to_svc(POSIX, Z_PORT_CONNECTED, 0, z_obj_uint32(7));
		} else if (subj == Z_PORT_DATA) {
			z_blob_t b;
			for (uint32_t i = 0; i < n && px.n < (int)sizeof px.in - 1; i++) px.in[px.n++] = (char)d[i];
			px.in[px.n] = 0;
			to_svc(POSIX, Z_PORT_DATA_ACK, tag, z_obj_none());
			if (!px.replied && strstr(px.in, "echo hi\r")) {
				// one DATA of 4 KB, as posix sends (OUT_BATCH): all of
				// it must arrive, the end last
				static char big[4096];
				for (int i = 0; i < 4090; i++) big[i] = (char)('0' + i % 10);
				memcpy(big + 4090, "hi\r\n$ ", 6);
				px.replied = true;
				to_svc(POSIX, Z_PORT_DATA, 7, blob_of(&b, big, sizeof big));
			}
		} else if (subj == Z_PORT_CLOSE) {
			px.closed = true;
		}
	}
}

z_rv z_msg_new_send(uint32_t to, uint32_t subject, uint32_t tag, z_obj_t obj) {
	from_svc(to, subject, tag, obj);
	return Z_OK;
}

// -- zport, as far as the service uses it -------------------------------------------

z_rv z_port_send(z_port_t *p, const void *d, uint32_t n) {
	z_blob_t b;
	if (!p->connected || p->pending_count >= Z_PORT_MAX_PENDING_SENDS) return Z_FAIL;
	p->pending_count++;
	from_svc(p->peer_pid, Z_PORT_DATA, p->conn_id, blob_of(&b, d, n));
	return Z_OK;
}

void z_port_send_ack(const z_msg_t *m) {
	if (m->obj.type != Z_BLOB) return;
	from_svc(m->from, Z_PORT_DATA_ACK, m->tag, z_obj_none());
}

void z_port_handle_ack(z_port_t *p, const z_msg_t *m) {
	if (p->connected && m->tag == p->conn_id && p->pending_count) p->pending_count--;
}

void z_port_handle_ack_closed(z_port_t *p, const z_msg_t *m) {
	if (!p->connected && m->tag == p->conn_id && p->pending_count) p->pending_count--;
}

void z_port_accept(z_port_t *p, const z_msg_t *m, uint32_t id) {
	memset(p, 0, sizeof *p);
	p->peer_pid = m->from;
	p->conn_id = id;
	p->connected = true;
	from_svc(m->from, Z_PORT_CONNECTED, 0, z_obj_uint32(id));
}

void z_port_refuse(const z_msg_t *m, const char *why) {
	from_svc(m->from, Z_PORT_REFUSED, 0, z_obj_str(why));
}

void z_port_close(z_port_t *p) {
	if (p->connected) from_svc(p->peer_pid, Z_PORT_CLOSE, p->conn_id, z_obj_none());
	p->connected = false;
}

void z_port_forget(z_port_t *p) {
	p->pending_count = 0;
	p->connected = false;
}

void z_port_reject_stranger(const z_msg_t *m) {
	z_port_send_ack(m);
	from_svc(m->from, Z_PORT_CLOSE, m->tag, z_obj_none());
}

bool z_port_peer_gone(const z_port_t *p) { (void)p; return false; }
bool z_port_pid_running(uint32_t pid) { (void)pid; return true; }

void z_port_ident(const z_msg_t *m, z_port_ident_t *out) {
	memset(out, 0, sizeof *out);
	out->remote = m->obj.type == Z_MAP;
	out->authenticated = true;
}

bool z_port_refuse_unauthenticated(const z_msg_t *m, const char *who) { (void)m; (void)who; return false; }
z_rv z_port_connect_arg(z_port_t *p, uint32_t pid, z_obj_t arg) { (void)p; (void)pid; (void)arg; return Z_FAIL; }
z_rv z_port_drain(z_port_t *p, uint32_t t) { (void)p; (void)t; return Z_OK; }

// -- the rest of the OS -----------------------------------------------------------

bool z_pid_register(const char *base, char *out, uint32_t len) {
	snprintf(out, len, "%s0", base);
	return true;
}

bool z_pid_lookup(const char *name, uint32_t *pid) {
	if (!strcmp(name, "posix0") && has_posix[me]) { *pid = POSIX; return true; }
	return false;
}

uint32_t z_proc_run(const char *name) { (void)name; return 0; }
void z_launch_arg_set(const char *arg) { (void)arg; }
bool z_launch_arg_take(char *out, int outlen) { (void)out; (void)outlen; return false; }

void z_rng_bytes(void *buf, uint32_t len) {
	for (uint32_t i = 0; i < len; i++) ((uint8_t *)buf)[i] = (uint8_t)random();
}

int z_auth_status(z_auth_status_t *st) {
	st->flags = has_pw[me] ? Z_AUTH_HAS_PASSWORD : 0;
	return Z_AUTH_OK;
}

int z_auth_check(const char *pw, uint32_t len, uint32_t *wait_ms) {
	if (wait_ms) *wait_ms = 0;
	return (has_pw[me] && len == 6 && !memcmp(pw, "secret", 6)) ? Z_AUTH_OK : Z_AUTH_E_BAD;
}

void uart_putc(char c) {
	static char line[256];
	static int n;
	if (c == '\r') return;
	if (c == '\n' || n == (int)sizeof line - 1) {
		line[n] = 0;
		fprintf(stderr, "    [%c] %s\n", 'A' + me, line);
		n = 0;
		return;
	}
	line[n++] = c;
}

// -- files: a directory per machine ----------------------------------------------------

static void host_path(char *out, size_t cap, int m, const char *p) {
	snprintf(out, cap, "%s%s%s", root[m], p[0] == '/' ? "" : "/", p);
}

int fs_open_read(const char *f) {
	char p[512];
	host_path(p, sizeof p, me, f);
	return open(p, O_RDONLY);
}

int fs_open_write(const char *f) {
	char p[512];
	host_path(p, sizeof p, me, f);
	return open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
}

int fs_read_chunk(int h, void *buf, int max) { return (int)read(h, buf, (size_t)max); }
int fs_write_chunk(int h, const void *buf, int len) { return (int)write(h, buf, (size_t)len); }
int fs_close_handle(int h) { return close(h) == 0; }

int fs_unlink(char *f) {
	char p[512];
	host_path(p, sizeof p, me, f);
	return unlink(p) == 0;
}

int fs_stat(const char *f, z_fs_info_t *info) {
	char p[512];
	struct stat st;
	host_path(p, sizeof p, me, f);
	if (stat(p, &st)) return 0;
	memset(info, 0, sizeof *info);
	info->type = S_ISDIR(st.st_mode) ? Z_FS_TYPE_DIR : Z_FS_TYPE_FILE;
	info->size = S_ISDIR(st.st_mode) ? 0 : (uint32_t)st.st_size;
	return 1;
}

int fs_list_ex(const char *path, char *buf, uint32_t cap, z_fs_info_t *info, uint32_t max,
               uint32_t *count, uint32_t *trunc) {
	char p[512];
	uint32_t used = 0, n = 0;
	struct dirent *e;
	host_path(p, sizeof p, me, path);
	DIR *d = opendir(p);
	if (!d) return 0;
	if (trunc) *trunc = 0;
	while ((e = readdir(d))) {
		char full[300], hp[800];
		if (e->d_name[0] == '.') continue;
		snprintf(full, sizeof full, "%s/%s", strcmp(path, "/") ? path : "", e->d_name);
		if (n == max || used + strlen(full) + 1 > cap) { if (trunc) *trunc = 1; break; }
		strcpy(buf + used, full);
		used += (uint32_t)strlen(full) + 1;
		snprintf(hp, sizeof hp, "%s/%s", p, e->d_name);
		fs_stat(full, &info[n]);
		n++;
	}
	closedir(d);
	*count = n;
	return 1;
}

// =========================================================================
// The scripts
// =========================================================================

enum {
	OP_END, OP_WAIT_UP, OP_WAIT_STREAM, OP_CMD, OP_SAME, OP_FLAG, OP_WAIT_FLAG,
	OP_TERM_OPEN, OP_TERM_REFUSED, OP_TERM_TYPE, OP_TERM_SEE, OP_TERM_CLOSE,
	OP_POSIX_OK, OP_WAIT_SOFT,
};

typedef struct {
	int op;
	const char *a;		// command, text, file
	int n;				// rc, flag, rate
	const char *b;		// text expected, second file
} op_t;

static const op_t *script;
static int pc;
static double op_t0;
static bool stopped;	// serve() has returned

static bool same(const char *fa, const char *fb) {
	// "A:/x" against "B:/y"
	char pa[512], pb[512];
	host_path(pa, sizeof pa, fa[0] - 'A', fa + 2);
	host_path(pb, sizeof pb, fb[0] - 'A', fb + 2);
	FILE *x = fopen(pa, "rb"), *y = fopen(pb, "rb");
	bool ok = x && y;
	while (ok) {
		int c = fgetc(x), d = fgetc(y);
		if (c != d) ok = false;
		if (c == EOF || d == EOF) break;
	}
	if (x) fclose(x);
	if (y) fclose(y);
	return ok;
}

static void step(void) {
	static bool busy_cmd;
	static int depth;
	if (depth) return;
	depth++;
	for (;;) {
		const op_t *o = &script[pc];
		bool done = false;
		if (sh->violations) fail("wires: %s", sh->violation);
		if (!op_t0) op_t0 = real_ms();
		if (real_ms() - op_t0 > 90000) {
			char what[160];
			snprintf(what, sizeof what, "op %d (%d %s) timed out; cli out: %.40s; term: %.40s", pc, o->op,
			         o->a ? o->a : "", cli_a.out, term.out);
			fail("%s", what);
		}
		switch (o->op) {
		case OP_END:
			lock();
			if (sh->violations) { unlock(); fail("wires: %s", sh->violation); }
			unlock();
			fprintf(stderr, "    [%c] done: %u frames out, %u in, %u resent, %u bad\n", 'A' + me,
			        L.st.tx_frames, L.st.rx_frames, L.st.retransmits, L.st.crc_errors);
			exit(0);
		case OP_WAIT_UP:
			done = zl_up(&L);
			break;
		case OP_WAIT_STREAM:
			done = zl_up(&L) && g_tr == T_STREAM && up.state == UP_DONE && g_rate == o->n;
			break;
		case OP_WAIT_SOFT:
			done = g_tr == T_SOFT && up.state == UP_IDLE && !zl_up(&L);
			break;
		case OP_CMD:
			if (!busy_cmd) {
				memset(&cli_a, 0, sizeof cli_a);
				cli_a.rc = -1;
				to_svc(CLI, Z_PORT_CONNECT, 0, z_obj_str(o->a));
				busy_cmd = true;
			} else if (cli_a.closed || (stopped && !strcmp(o->a, "stop"))) {
				busy_cmd = false;
				fprintf(stderr, "    [%c] $ zlink %s  (rc %d, %.1f s)\n%s", 'A' + me, o->a, cli_a.rc,
				        (real_ms() - op_t0) / 1000.0, cli_a.out);
				if (cli_a.rc != o->n) fail("wrong exit status for: %s", o->a);
				if (o->b && !strstr(cli_a.out, o->b)) fail("expected text missing: %s", o->b);
				done = true;
			}
			break;
		case OP_SAME:
			if (!same(o->a, o->b)) fail("files differ: %s", o->a);
			done = true;
			break;
		case OP_FLAG:
			sh->flag[me] = o->n;
			done = true;
			break;
		case OP_WAIT_FLAG:
			done = sh->flag[1 - me] >= o->n;
			break;
		case OP_TERM_OPEN:
		case OP_TERM_REFUSED:
			if (!term.connected && !term.refused && !busy_cmd) {
				memset(&term, 0, sizeof term);
				to_svc(TERM, Z_PORT_CONNECT, 0, z_obj_none());
				busy_cmd = true;
			}
			if (term.connected || term.refused) {
				busy_cmd = false;
				if (term.refused != (o->op == OP_TERM_REFUSED)) fail("term: unexpected answer%s", NULL);
				done = true;
			}
			break;
		case OP_TERM_TYPE: {
			z_blob_t b;
			to_svc(TERM, Z_PORT_DATA, term.conn, blob_of(&b, o->a, (uint32_t)strlen(o->a)));
			done = true;
			break;
		}
		case OP_TERM_SEE:
			if (strstr(term.out, o->a)) {
				if (o->n && term.n < o->n) fail("term: output cut short%s", NULL);
				memset(term.out, 0, sizeof term.out);
				term.n = 0;
				done = true;
			}
			break;
		case OP_TERM_CLOSE:
			to_svc(TERM, Z_PORT_CLOSE, term.conn, z_obj_none());
			done = true;
			break;
		case OP_POSIX_OK:
			done = px.connected && px.map_ok && px.closed && strstr(px.in, "echo hi\r");
			break;
		}
		if (!done) break;
		pc++;
		op_t0 = real_ms();
	}
	depth--;
}

// -- scenario 1: soft, B has no engine ------------------------------------------------

static const op_t s1_a[] = {
	{ OP_WAIT_UP, 0, 0, 0 },
	{ OP_CMD, "status", 0, "up, soft" },
	{ OP_CMD, "ls /", 0, "b.txt  500" },
	{ OP_CMD, "get /b.txt /copy.txt", 0, "bytes in" },
	{ OP_SAME, "A:/copy.txt", 0, "B:/b.txt" },
	{ OP_CMD, "put /hello.txt /x.txt", 1, "only lends its files to read" },
	{ OP_CMD, "get /nope", 1, "no such file" },
	{ OP_CMD, "get", 2, "usage" },
	{ OP_FLAG, 0, 1, 0 },
	{ OP_WAIT_FLAG, 0, 1, 0 },
	{ OP_CMD, "speed 4", 0, "KB/s" },
	{ OP_TERM_OPEN, 0, 0, 0 },
	{ OP_TERM_SEE, "password: ", 0, 0 },
	{ OP_TERM_TYPE, "wrong\r", 0, 0 },
	{ OP_TERM_SEE, "wrong password", 0, 0 },
	{ OP_TERM_TYPE, "secret\r", 0, 0 },
	{ OP_TERM_TYPE, "echo hi\r", 0, 0 },
	{ OP_TERM_SEE, "hi\r\n$ ", 4096, 0 },	// all 4 KB of it
	{ OP_TERM_CLOSE, 0, 0, 0 },
	{ OP_FLAG, 0, 2, 0 },
	{ OP_WAIT_FLAG, 0, 2, 0 },
	{ OP_END, 0, 0, 0 },
};

static const op_t s1_b[] = {
	{ OP_WAIT_UP, 0, 0, 0 },
	{ OP_WAIT_FLAG, 0, 1, 0 },
	{ OP_CMD, "get /hello.txt /h.txt", 0, "bytes in" },
	{ OP_SAME, "B:/h.txt", 0, "A:/hello.txt" },
	{ OP_CMD, "put /b.txt /from_b.txt", 0, "bytes in" },
	{ OP_SAME, "A:/from_b.txt", 0, "B:/b.txt" },
	{ OP_TERM_REFUSED, 0, 0, 0 },			// A has no password: no shell
	{ OP_CMD, "status", 0, "no stream engine" },
	{ OP_FLAG, 0, 1, 0 },
	{ OP_WAIT_FLAG, 0, 2, 0 },
	{ OP_POSIX_OK, 0, 0, 0 },
	{ OP_FLAG, 0, 2, 0 },
	{ OP_END, 0, 0, 0 },
};

// -- scenario 2: both have engines, crossed cable ----------------------------------------

static const op_t s2_a[] = {
	{ OP_WAIT_STREAM, 0, 12000, 0 },
	{ OP_CMD, "status", 0, "stream at 12000 kbit/s" },
	{ OP_CMD, "get /big.bin /big2.bin", 0, "bytes in" },
	{ OP_SAME, "A:/big2.bin", 0, "B:/big.bin" },
	{ OP_CMD, "put /big.bin /big3.bin", 0, "bytes in" },
	{ OP_SAME, "B:/big3.bin", 0, "A:/big.bin" },
	{ OP_CMD, "speed 1024", 0, "KB/s" },
	{ OP_FLAG, 0, 1, 0 },
	{ OP_WAIT_FLAG, 0, 1, 0 },				// B has stopped
	{ OP_WAIT_SOFT, 0, 0, 0 },				// and A is back on soft, alone
	{ OP_END, 0, 0, 0 },
};

static const op_t s2_b[] = {
	{ OP_WAIT_STREAM, 0, 12000, 0 },
	{ OP_WAIT_FLAG, 0, 1, 0 },
	{ OP_CMD, "stop", 0, "stopped" },
	{ OP_FLAG, 0, 1, 0 },
	{ OP_END, 0, 0, 0 },
};

// -- scenario 3: the stream fails above 3 Mbit/s ---------------------------------------------

static const op_t s3_a[] = {
	{ OP_WAIT_STREAM, 0, 3000, 0 },
	{ OP_CMD, "get /b.txt /c.txt", 0, "bytes in" },
	{ OP_SAME, "A:/c.txt", 0, "B:/b.txt" },
	{ OP_FLAG, 0, 1, 0 },
	{ OP_WAIT_FLAG, 0, 1, 0 },
	{ OP_END, 0, 0, 0 },
};

static const op_t s3_b[] = {
	{ OP_WAIT_STREAM, 0, 3000, 0 },
	{ OP_WAIT_FLAG, 0, 1, 0 },
	{ OP_FLAG, 0, 1, 0 },
	{ OP_END, 0, 0, 0 },
};

// -- scenario 4: -m stream on fixed pins ------------------------------------------------------

static const op_t s4_a[] = {
	{ OP_WAIT_STREAM, 0, 6000, 0 },
	{ OP_CMD, "get /big.bin /big4.bin", 0, "bytes in" },
	{ OP_SAME, "A:/big4.bin", 0, "B:/big.bin" },
	{ OP_FLAG, 0, 1, 0 },
	{ OP_WAIT_FLAG, 0, 1, 0 },
	{ OP_END, 0, 0, 0 },
};

static const op_t s4_b[] = {
	{ OP_WAIT_STREAM, 0, 6000, 0 },
	{ OP_WAIT_FLAG, 0, 1, 0 },
	{ OP_FLAG, 0, 1, 0 },
	{ OP_END, 0, 0, 0 },
};

// -- scenario 5: the first answer is lost ------------------------------------------------------

static const op_t s5[] = {
	{ OP_WAIT_STREAM, 0, 6000, 0 },		// B stepped down after its failed try
	{ OP_FLAG, 0, 1, 0 },
	{ OP_WAIT_FLAG, 0, 1, 0 },
	{ OP_END, 0, 0, 0 },
};

// =========================================================================

static void write_file(int m, const char *name, const void *d, size_t n) {
	char p[512];
	host_path(p, sizeof p, m, name);
	FILE *f = fopen(p, "wb");
	fwrite(d, 1, n, f);
	fclose(f);
}

typedef struct {
	const char *name;
	int engines[2], crossed;
	uint32_t max_kbit;
	const char *args[2];
	const op_t *ops[2];
	bool pw[2], posix[2];
	int lose;			// upgrade answers to lose, whichever machine answers
} scenario_t;

static int run_machine(const scenario_t *s, int m) {
	char line[128], buf[128];
	char *argv[12];
	uint8_t flags[12];
	me = m;
	srandom((unsigned)getpid());
	script = s->ops[m];
	has_pw[0] = s->pw[0];
	has_pw[1] = s->pw[1];
	has_posix[0] = s->posix[0];
	has_posix[1] = s->posix[1];
	zl_test_lose_p = &sh->lose;
	snprintf(line, sizeof line, "%s", s->args[m]);
	int argc = z_args_split(line, buf, sizeof buf, argv, flags, 12);
	int rc = serve(argc, argv);
	stopped = true;
	// the script may go on after a `zlink stop`
	for (int i = 0; i < 100000; i++) { step(); usleep(1000); }
	return rc ? rc : 1;
}

static bool run(const scenario_t *s) {
	pid_t p[2];
	int st, ok = 1;
	double t0 = real_ms();
	memset(sh, 0, sizeof *sh);
	sh->engines[0] = s->engines[0];
	sh->engines[1] = s->engines[1];
	sh->crossed = s->crossed;
	sh->max_kbit = s->max_kbit;
	sh->lose = s->lose;
	fprintf(stderr, "%s\n", s->name);
	for (int m = 0; m < 2; m++) {
		p[m] = fork();
		if (p[m] == 0) exit(run_machine(s, m));
	}
	for (int m = 0; m < 2; m++) {
		waitpid(p[m], &st, 0);
		if (!WIFEXITED(st) || WEXITSTATUS(st)) {
			ok = 0;
			kill(p[1 - m], SIGKILL);
		}
	}
	fprintf(stderr, "  %s (%.1f s)\n\n", ok ? "ok" : "FAILED", (real_ms() - t0) / 1000.0);
	return ok;
}

int main(void) {
	static uint8_t big[2][300000];
	char base[] = "/tmp/zlink_app_XXXXXX";
	int fails = 0;
	char text[600];

	if (!mkdtemp(base)) return 1;
	for (int m = 0; m < 2; m++) {
		snprintf(root[m], sizeof root[m], "%s/%c", base, 'A' + m);
		mkdir(root[m], 0755);
	}
	srandom(1);
	for (int m = 0; m < 2; m++)
		for (size_t i = 0; i < sizeof big[m]; i++) big[m][i] = (uint8_t)random();
	write_file(0, "/hello.txt", "hello from A\n", 13);
	write_file(0, "/big.bin", big[0], sizeof big[0]);
	for (int i = 0; i < 50; i++) memcpy(text + i * 10, "this is b\n", 10);
	write_file(1, "/b.txt", text, 500);
	write_file(1, "/big.bin", big[1], sizeof big[1]);

	sh = mmap(NULL, sizeof *sh, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (sh == MAP_FAILED) return 1;

	static const scenario_t sc[] = {
		{ "1. soft: B has no engine; files, refusals, speed, a shell",
		  { 1, 0 }, 0, 12000, { "-w", "-f" }, { s1_a, s1_b }, { false, true }, { false, true } },
		{ "2. both have engines, crossed cable: up to 12 Mbit/s, then B stops",
		  { 1, 1 }, 1, 12000, { "-w", "-w" }, { s2_a, s2_b }, { false, false }, { false, false } },
		{ "3. the stream only works up to 3 Mbit/s: the ladder steps down",
		  { 1, 1 }, 0, 3000, { "-f", "-f" }, { s3_a, s3_b }, { false, false }, { false, false } },
		{ "4. -m stream on fixed pins",
		  { 1, 1 }, 0, 12000, { "-f -m stream -r 6000 0 0 1", "-f -m stream -r 6000 0 1 0" },
		  { s4_a, s4_b }, { false, false }, { false, false } },
		{ "5. the answer to the first offer is lost",
		  { 1, 1 }, 0, 12000, { "-f", "-f" }, { s5, s5 }, { false, false }, { false, false }, 1 },
	};
	for (unsigned i = 0; i < sizeof sc / sizeof sc[0]; i++)
		if (!run(&sc[i])) fails++;

	char cmd[300];
	snprintf(cmd, sizeof cmd, "rm -rf %s", base);
	if (system(cmd)) { /* nothing */ }
	if (fails) {
		fprintf(stderr, "test_zlink_app: %d FAILED\n", fails);
		return 1;
	}
	fprintf(stderr, "test_zlink_app: PASS\n");
	return 0;
}
