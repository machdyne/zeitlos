/*
 * `run fed key`, `run fed nodes`, `run fed add ...` -- the Zeitlos side of
 * the node-list commands (zeitlos/fed.c's run_admin(), core/fadmin.c)
 * against a scripted kernel whose fed0 is a REAL Linux fed: each request
 * forwarded to its socket, each reply handed back as port DATA. What the
 * command prints to the terminal arrives at a scripted posix0.
 *
 *   make -C sw/apps/fed test
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#define main fed_main_unused
#include "../zeitlos/fed.c"
#undef main
#include "../../../common/zproc.h"

bool z_rng_secure(void) { return true; }
void z_rng_bytes(void *b, uint32_t n) { plat_random(b, n); }
bool z_launch_arg_take(char *out, int outlen) { (void)out; (void)outlen; return false; }
extern int plat_quiet;

#define FED   60
#define POSIX 90

static int checks, fails;
#define CK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// -- the mailbox --
#define QN 256
static struct { z_msg_t m; z_blob_t blob; uint8_t data[4096]; } q[QN];
static int qh, qt;
static z_obj_t k_ok, k_fail;
static void deliver(uint32_t from, uint32_t subject, uint32_t tag, z_obj_t obj) {
	int i = qt;
	qt = (qt + 1) % QN;
	if (qt == qh) { printf("FAIL: the test's mailbox overflowed\n"); exit(1); }
	memset(&q[i].m, 0, sizeof(q[i].m));
	q[i].m.from = from; q[i].m.subject = subject; q[i].m.tag = tag; q[i].m.obj = obj;
}
static void deliver_blob(uint32_t from, uint32_t subject, uint32_t tag, const void *d, uint32_t n) {
	int i = qt;
	memcpy(q[i].data, d, n);
	q[i].blob.len = n; q[i].blob.data = q[i].data;
	z_obj_t o; o.type = Z_BLOB; o.val.ptr = &q[i].blob;
	deliver(from, subject, tag, o);
}

static uint32_t now_ticks(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint32_t)((uint64_t)t.tv_sec * Z_TICK_HZ + (uint64_t)t.tv_nsec * Z_TICK_HZ / 1000000000u);
}

// -- fed0: a real fed, over its socket --
static char g_sock_path[300];
static int g_fd = -1;
static bool g_fed_up = true;			// false: nothing at fed0
// -- posix0: what the command says --
static char said[8192];
static int saidlen;

static uint8_t kv_seed[32];

static void on_send(z_msg_t *m) {
	if (m->to == FED) {
		if (m->subject == Z_PORT_CONNECT) {
			struct sockaddr_un ua;
			memset(&ua, 0, sizeof(ua));
			ua.sun_family = AF_UNIX;
			snprintf(ua.sun_path, sizeof(ua.sun_path), "%s", g_sock_path);
			g_fd = socket(AF_UNIX, SOCK_STREAM, 0);
			if (!g_fed_up || connect(g_fd, (struct sockaddr *)&ua, sizeof(ua))) {
				close(g_fd); g_fd = -1;
				deliver(FED, Z_PORT_REFUSED, 0, z_obj_none());
				return;
			}
			deliver(FED, Z_PORT_CONNECTED, 0, z_obj_uint32(5));
		} else if (m->subject == Z_PORT_DATA && m->obj.type == Z_BLOB) {
			z_blob_t *b = m->obj.val.ptr;
			if (g_fd >= 0 && write(g_fd, b->data, b->len) != (ssize_t)b->len) { }
			deliver(FED, Z_PORT_DATA_ACK, m->tag, z_obj_none());
		} else if (m->subject == Z_PORT_CLOSE) {
			if (g_fd >= 0) close(g_fd);
			g_fd = -1;
		}
		return;
	}
	if (m->to == POSIX) {
		if (m->subject == Z_PORT_CONNECT) deliver(POSIX, Z_PORT_CONNECTED, 0, z_obj_uint32(3));
		else if (m->subject == Z_PORT_DATA && m->obj.type == Z_BLOB) {
			z_blob_t *b = m->obj.val.ptr;
			if (saidlen + (int)b->len < (int)sizeof(said) - 1) { memcpy(said + saidlen, b->data, b->len); saidlen += (int)b->len; said[saidlen] = 0; }
			deliver(POSIX, Z_PORT_DATA_ACK, m->tag, z_obj_none());
		}
	}
}

static uint32_t *k_syscall(uint32_t id, uint32_t *args, uint32_t b) {
	(void)b;
	switch (id) {
	case Z_SYS_UPTIME:
		((z_obj_t *)args)->type = Z_UINT32; ((z_obj_t *)args)->val.uint32 = now_ticks();
		return (uint32_t *)&k_ok;
	case Z_SYS_MSG_SEND:
		on_send((z_msg_t *)args);
		return (uint32_t *)&k_ok;
	case Z_SYS_MSG_READ:
		// fed's replies, as they come off its socket
		if (g_fd >= 0) {
			struct pollfd pf = { g_fd, POLLIN, 0 };
			if (poll(&pf, 1, 1) > 0) {
				uint8_t buf[3000];
				ssize_t r = read(g_fd, buf, sizeof(buf));
				if (r > 0) deliver_blob(FED, Z_PORT_DATA, 5, buf, (uint32_t)r);
			}
		}
		if (qh == qt) return (uint32_t *)&k_fail;
		*(z_msg_t *)args = q[qh].m;
		qh = (qh + 1) % QN;
		return (uint32_t *)&k_ok;
	case Z_SYS_PROC_STATUS: {
		z_proc_status_args_t *a = (z_proc_status_args_t *)args;
		a->state = Z_PROC_STATE_RUNNING;
		return (uint32_t *)&k_ok;
	}
	case Z_SYS_PID_LOOKUP: {
		z_obj_t *o = (z_obj_t *)args;
		if (o->type == Z_STR && !strcmp(o->val.str, "fed0")) { o->type = Z_UINT32; o->val.uint32 = FED; return (uint32_t *)&k_ok; }
		if (o->type == Z_STR && !strcmp(o->val.str, "posix0")) { o->type = Z_UINT32; o->val.uint32 = POSIX; return (uint32_t *)&k_ok; }
		return (uint32_t *)&k_fail;
	}
	case Z_SYS_KV: {
		z_kv_args_t *a = (z_kv_args_t *)args;
		if (a->op == Z_KV_GET) { memcpy(a->val, kv_seed, 32); a->len = 32; a->result = Z_KV_OK; }
		return (uint32_t *)&k_ok;
	}
	default:
		break;
	}
	return (uint32_t *)&k_ok;
}

static bool k_install(void) {
	if ((uintptr_t)(void *)k_syscall > 0xFFFFFFFFu) return false;
	if (mmap((void *)0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == MAP_FAILED)
		return false;
	k_ok.type = Z_UINT32; k_ok.val.uint32 = Z_OK;
	k_fail.type = Z_UINT32; k_fail.val.uint32 = Z_FAIL;
	*(volatile uint32_t *)0x0000000c = (uint32_t)(uintptr_t)k_syscall;
	return true;
}

// `run fed <words>`: what it printed on the posix terminal, and its code
static int adm(const char *words, const char *dir) {
	saidlen = 0; said[0] = 0;
	memset(&g_out, 0, sizeof(g_out)); g_out_on = false;
	return run_admin(words, dir);
}

int main(int argc, char **argv) {
	const char *fed = argc > 1 ? argv[1] : "./fed-linux";
	char dir[] = "/tmp/fed-zadmin-XXXXXX", cmd[600], hex[65], kb[65];
	if (!k_install()) { printf("zadmin: skipped (cannot map page 0 -- vm.mmap_min_addr)\n"); return 0; }
	signal(SIGPIPE, SIG_IGN);
	if (!mkdtemp(dir)) return 1;
	char bdir[340];
	snprintf(bdir, sizeof(bdir), "%s/b", dir);
	snprintf(cmd, sizeof(cmd), "mkdir -p %s && %s --dir %s --print-key >/dev/null && %s --dir %s --print-key > %s/kb", bdir, fed, dir, fed, bdir, dir);
	if (system(cmd)) return 1;
	snprintf(cmd, sizeof(cmd), "%s/kb", dir);
	FILE *f = fopen(cmd, "r"); if (!f || !fgets(kb, sizeof(kb), f)) return 1; fclose(f);
	snprintf(cmd, sizeof(cmd), "%s/node.key", dir);
	f = fopen(cmd, "rb"); if (!f || fread(kv_seed, 1, 32, f) != 32) return 1; fclose(f);
	{	// from a copy: crypto_ed25519_key_pair() wipes the seed it is given
		uint8_t sk[64], pk[32], seed[32];
		memcpy(seed, kv_seed, 32);
		crypto_ed25519_key_pair(sk, pk, seed);
		fobj_hex(pk, 32, hex);
	}
	snprintf(cmd, sizeof(cmd), "%s/fed.cfg", dir);
	f = fopen(cmd, "w");
	fprintf(f, "name: alpha\nsubscribe: t/*\nnetwork: t %s\n", hex);
	fclose(f);
	snprintf(g_sock_path, sizeof(g_sock_path), "%s/fed.sock", dir);
	pid_t pid = fork();
	if (!pid) { int nul = open("/dev/null", O_WRONLY); dup2(nul, 2); execl(fed, fed, "--dir", dir, (char *)NULL); _exit(1); }
	for (int i = 0; i < 100 && access(g_sock_path, F_OK); i++) usleep(50000);
	plat_quiet = 1;

	int rc = adm("key", dir);
	CK(rc == 0 && strstr(said, "PUBLIC key") && strstr(said, hex) && strstr(said, "key/value store"),
		"run fed key: the public key, on the posix terminal; the private one only named (%s)", said);
	CK(strstr(said, "\r\n") != NULL, "lines end \\r\\n, as a terminal wants");

	rc = adm("nodes", dir);
	CK(rc == 0 && strstr(said, "no list yet"), "run fed nodes: none yet (%s)", said);

	char words[300];
	kb[64] = 0;
	snprintf(words, sizeof(words), "add beta %s sysop=bob", kb);
	rc = adm(words, dir);
	CK(rc == 0 && strstr(said, "published: t now has 2 nodes") && strstr(said, "as alpha"),
		"run fed add beta ...: the first list, published through the real fed (%s)", said);
	rc = adm("nodes", dir);
	CK(rc == 0 && strstr(said, "beta") && strstr(said, "bob") && strstr(said, "(this node)"), "run fed nodes: both (%s)", said);
	// and the real fed agrees
	snprintf(cmd, sizeof(cmd), "%s --dir %s nodes 2>&1", fed, dir);
	FILE *p = popen(cmd, "r");
	char linux_says[2000] = "";
	size_t ln = fread(linux_says, 1, sizeof(linux_says) - 1, p);
	linux_says[ln] = 0;
	pclose(p);
	CK(strstr(linux_says, "beta") && strstr(linux_says, "bob"), "the Linux side's `fed nodes` shows beta too");
	rc = adm("remove beta", dir);
	CK(rc == 0 && strstr(said, "now has 1 node"), "run fed remove beta (%s)", said);
	rc = adm("add gamma 0123", dir);
	CK(rc == 1 && strstr(said, "not a public key"), "a bad key: refused, and said why (%s)", said);

	kill(pid, SIGTERM);
	waitpid(pid, NULL, 0);
	g_fed_up = false;
	rc = adm("nodes", dir);
	CK(rc == 1 && strstr(said, "not running") && strstr(said, "run fed"), "fed not running: said so, and how to start it (%s)", said);

	printf("zadmin: %d checks, %d failed\n", checks, fails);
	snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
	if (!fails && system(cmd)) { }
	return fails != 0;
}
