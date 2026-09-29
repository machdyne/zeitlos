/*
 * Host test for bbs.c's link to fed0 (docs/bbs.md, "Federation"): the
 * REAL bbs.c and core, this file playing the kernel and a scripted fed --
 * and term, for one caller whose connection id is the same as fed's.
 * (tests/test_zport.c is the port provider itself.)
 * Files go through the Linux platform (linux/plat.c) into /tmp.
 *
 *   make -C sw/apps/bbs test      (needs vm.mmap_min_addr=0; skips otherwise)
 *
 * Exit 0 pass, 1 fail, 77 skipped.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define main bbs_main_unused
#include "../bbs.c"
#undef main
#include "../core/bbs_int.h"

// bbs.c's main() (not run here) is the only caller of these.
bool z_rng_secure(void) { return true; }
void z_rng_bytes(void *b, uint32_t n) { memset(b, 7, n); }

extern int plat_quiet;

#define TERM     30
#define NETSERVE 40
#define STRANGER 99
#define FED      60

// -- what bbs sent --
typedef struct { uint32_t to, subject, tag, type, u32; uint8_t data[1100]; uint32_t len; char str[80]; } out_t;
static out_t out[4000];
static int nout;

// -- what it will read --
static z_msg_t q[64];
static int qn;
static z_blob_t qblob[64];
static uint8_t qdata[64][600];

static uint32_t ticks = 1000, dead_pid;
static z_obj_t k_ok, k_fail;

static void deliver(uint32_t from, uint32_t subject, uint32_t tag, z_obj_t obj) {
	memset(&q[qn], 0, sizeof(q[qn]));
	q[qn].from = from; q[qn].subject = subject; q[qn].tag = tag; q[qn].obj = obj;
	qn++;
}

static void deliver_data(uint32_t from, uint32_t tag, const char *s) {
	uint32_t n = (uint32_t)strlen(s);
	memcpy(qdata[qn], s, n);
	qblob[qn].len = n; qblob[qn].data = qdata[qn];
	z_obj_t o; o.type = Z_BLOB; o.val.ptr = &qblob[qn];
	deliver(from, Z_PORT_DATA, tag, o);
}

static uint32_t *k_syscall(uint32_t id, uint32_t *args, uint32_t b) {
	(void)b;
	switch (id) {
	case Z_SYS_UPTIME:
		((z_obj_t *)args)->type = Z_UINT32; ((z_obj_t *)args)->val.uint32 = ticks;
		return (uint32_t *)&k_ok;
	case Z_SYS_MSG_SEND: {
		z_msg_t *m = (z_msg_t *)args;
		out_t *o = &out[nout++];
		memset(o, 0, sizeof(*o));
		o->to = m->to; o->subject = m->subject; o->tag = m->tag; o->type = m->obj.type;
		if (m->obj.type == Z_UINT32) o->u32 = m->obj.val.uint32;
		if (m->obj.type == Z_BLOB) {
			z_blob_t *bl = m->obj.val.ptr;
			o->len = bl->len < sizeof(o->data) ? bl->len : sizeof(o->data);
			memcpy(o->data, bl->data, o->len);
		}
		if (m->obj.type == Z_STR && m->obj.val.str) snprintf(o->str, sizeof(o->str), "%s", m->obj.val.str);
		return (uint32_t *)&k_ok;
	}
	case Z_SYS_MSG_READ:
		if (!qn) return (uint32_t *)&k_fail;
		*(z_msg_t *)args = q[0];
		memmove(q, q + 1, sizeof(z_msg_t) * (size_t)(--qn));
		return (uint32_t *)&k_ok;
	case Z_SYS_PID_LOOKUP: {
		z_obj_t *o = (z_obj_t *)args;
		if (o->type == Z_STR && !strcmp(o->val.str, "fed0") && dead_pid != FED) { o->type = Z_UINT32; o->val.uint32 = FED; return (uint32_t *)&k_ok; }
		return (uint32_t *)&k_fail;
	}
	case Z_SYS_PROC_STATUS: {
		z_proc_status_args_t *a = (z_proc_status_args_t *)args;
		a->state = a->pid == dead_pid ? Z_PROC_STATE_EXITED : Z_PROC_STATE_RUNNING;
		return (uint32_t *)&k_ok;
	}
	default:
		return (uint32_t *)&k_ok;
	}
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

static int checks, fails;
#define CK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %d: ", __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

// The BBS's own clock (plat_ms()) follows the kernel's ticks, as on a
// board -- the fake one zplat_posix.c keeps for tests; the date stays today.
extern int plat_fake_time;
extern uint32_t plat_fake_now, plat_fake_ms;
static void steps(int n) {
	while (n--) {
		plat_fake_ms = (uint32_t)((uint64_t)ticks * 1000u / Z_TICK_HZ);
		step();
	}
}

static int find(uint32_t to, uint32_t subject, int from) {
	for (int i = from; i < nout; i++) if (out[i].to == to && out[i].subject == subject) return i;
	return -1;
}

// Everything bbs sent `to` on `tag` since `from`, concatenated; acked as
// term or netserve would ack it (unless `ack` is false).
static uint32_t collect(uint32_t to, uint32_t tag, int from, char *buf, uint32_t cap, bool ack) {
	uint32_t n = 0;
	for (int i = from; i < nout; i++)
		if (out[i].to == to && out[i].subject == Z_PORT_DATA && out[i].tag == tag) {
			if (n + out[i].len < cap) { memcpy(buf + n, out[i].data, out[i].len); n += out[i].len; }
			if (ack) deliver(to, Z_PORT_DATA_ACK, tag, z_obj_none());
		}
	buf[n] = 0;
	return n;
}


// Everything bbs sent to fed since `from`, concatenated, acked as fed would.
static uint32_t to_fed(int from, char *buf, uint32_t cap, uint32_t id) {
	return collect(FED, id, from, buf, cap, true);
}

int main(void) {
	static char buf[65536];
	char dir[64];
	int mark;
	const char *self = "aaaa000000000000000000000000000000000000000000000000000000000001";
	const char *other = "bbbb111111111111111111111111111111111111111111111111111111111111";

	if (!k_install()) { printf("bbs fed-link test: skipped (cannot map page 0)\n"); return 77; }
	plat_quiet = getenv("BBS_TEST_LOG") ? 0 : 1;
	plat_fake_time = 1;
	plat_fake_now = (uint32_t)time(NULL);
	plat_fake_ms = (uint32_t)((uint64_t)ticks * 1000u / Z_TICK_HZ);
	snprintf(dir, sizeof(dir), "/tmp/bbs-zfed-%d", (int)getpid());
	mkdir(dir, 0755);
	{ char p[128]; snprintf(p, sizeof(p), "%s/bbs.cfg", dir);
	  FILE *f = fopen(p, "w"); fputs("name: Port BBS\nnodes: 2\nfed: fed0\n", f); fclose(f);
	  snprintf(p, sizeof(p), "%s/forums.cfg", dir);
	  f = fopen(p, "w"); fputs("general; General; 0; 10; Anything; t/forum/general\n", f); fclose(f); }
	CK(bbs_init(dir), "the core starts, with a federated forum");

	// -- 1. it connects to fed0, and asks --
	mark = nout;
	steps(2);
	int c = find(FED, Z_PORT_CONNECT, mark);
	CK(c >= 0, "a CONNECT to fed0");
	mark = nout;
	deliver(FED, Z_PORT_CONNECTED, 0, z_obj_uint32(1));		// id 1: the same as term's, below
	steps(2);
	to_fed(mark, buf, sizeof(buf), 1);
	CK(strstr(buf, "KEY\n") && strstr(buf, "SUB bbs t/*\n"), "then KEY and SUB bbs t/* (%s)", buf);

	// -- 2. a post delivered, in two pieces: stored, acknowledged --
	{
		char payload[300], obj[1200], line[40];
		int pl = snprintf(payload, sizeof(payload), "{\"from\":\"carol\",\"subject\":\"Hello\",\"date\":%u,\"body\":\"from far away\"}",
			(unsigned)plat_now());
		int ol = snprintf(obj, sizeof(obj), "ZFED1\ntopic: t/forum/general\ntype: bbs.post\nformat: json\nkind: log\n"
			"origin: %s\ntime: %u\nseq: 1\nlen: %d\n\n%s\nsig: %0128d\n", other, (unsigned)plat_now(), pl, payload, 0);
		char hello[200];
		snprintf(hello, sizeof(hello), "OK %s alpha\nOK subscribed after 0\n", self);
		mark = nout;
		deliver_data(FED, 1, hello);
		steps(1);
		int ln = snprintf(line, sizeof(line), "OBJ 7 %d\n", ol);
		static char first[1300];
		memcpy(first, line, (size_t)ln);
		memcpy(first + ln, obj, 100);
		first[ln + 100] = 0;
		deliver_data(FED, 1, first);				// the OBJ line and the start ...
		steps(1);
		CK(msg_count(1) == 0, "half an object: nothing stored yet");
		deliver_data(FED, 1, obj + 100);			// ... and the rest
		steps(2);
		msg_t m; idx_t e;
		CK(msg_count(1) == 1 && msg_head(1, 1, &m, &e) && !strcmp(m.from, "carol@bbbb111111111111") &&
			strlen(m.id) == 64, "stored: carol@<its short id>, under its object id (%s)", m.from);
		to_fed(mark, buf, sizeof(buf), 1);
		CK(strstr(buf, "ACK 7\n") != NULL, "and acknowledged");
	}

	// -- 3. a post of ours goes to fed --
	{
		msg_t m;
		memset(&m, 0, sizeof(m));
		snprintf(m.from, sizeof(m.from), "phil");
		snprintf(m.subject, sizeof(m.subject), "Mine");
		mark = nout;
		CK(fed_post(1, &m, "from here", 9) == 1, "a post: on its way");
		steps(2);
		to_fed(mark, buf, sizeof(buf), 1);
		CK(strstr(buf, "PUB t/forum/general bbs.post json log - ") && strstr(buf, "from here"), "PUBlished to fed");
	}

	// -- 4. a caller whose connection id is fed's: told apart by sender --
	mark = nout;
	deliver(TERM, Z_PORT_CONNECT, 0, z_obj_none());
	steps(2);
	c = find(TERM, Z_PORT_CONNECTED, mark);
	uint32_t tc = c >= 0 ? out[c].u32 : 0;
	CK(tc == 1, "term's connection id is 1, like fed's (%u)", (unsigned)tc);
	collect(TERM, tc, mark, buf, sizeof(buf), true);
	mark = nout;
	deliver_data(TERM, tc, "\x1b[1;2R");
	steps(2);
	deliver_data(TERM, tc, "\x1b[25;80R");
	steps(3);
	collect(TERM, tc, mark, buf, sizeof(buf), true);
	CK(strstr(buf, "Welcome to") != NULL, "term's data reaches its caller, not the fed link");
	to_fed(mark, buf, sizeof(buf), 1);
	CK(!strstr(buf, "1;2R"), "and none of it went to fed");

	// -- 5. fed closes: back after 5 seconds, the unanswered post again --
	mark = nout;
	deliver(FED, Z_PORT_CLOSE, 1, z_obj_none());
	steps(2);
	CK(find(FED, Z_PORT_CONNECT, mark) < 0, "fed closed: not at once");
	ticks += 6 * Z_TICK_HZ;
	steps(2);
	c = find(FED, Z_PORT_CONNECT, mark);
	CK(c >= 0, "five seconds on: a CONNECT again");
	mark = nout;
	deliver(FED, Z_PORT_CONNECTED, 0, z_obj_uint32(2));
	steps(2);
	to_fed(mark, buf, sizeof(buf), 2);
	CK(strstr(buf, "SUB bbs t/*") && strstr(buf, "from here"), "SUB again, and the post fed never answered, sent again");

	// -- 6. fed dies: noticed, and back when it is --
	mark = nout;
	dead_pid = FED;
	ticks += 2 * Z_TICK_HZ;
	steps(2);
	ticks += 6 * Z_TICK_HZ;
	steps(2);
	CK(find(FED, Z_PORT_CONNECT, mark) < 0, "fed gone: no CONNECT to a pid that is not there");
	dead_pid = 0;
	ticks += 6 * Z_TICK_HZ;
	steps(2);
	CK(find(FED, Z_PORT_CONNECT, mark) >= 0, "fed back: connected again");

	// -- 7. a letter fed will not send: said to its writer, in their mailbox --
	{
		mark = nout;
		deliver(FED, Z_PORT_CONNECTED, 0, z_obj_uint32(3));
		steps(2);
		char hello[240];
		snprintf(hello, sizeof(hello), "OK %s 0123456789abcdef alpha\nOK subscribed after 7\nOK %064d 9\n", self, 5);
		deliver_data(FED, 3, hello);				// KEY, SUB, and the post waiting since section 3
		steps(2);
		user_t u;
		memset(&u, 0, sizeof(u));
		snprintf(u.handle, sizeof(u.handle), "phil");
		u.level = 10;
		CK(users_add(&u), "(a user, phil)");
		msg_t m;
		memset(&m, 0, sizeof(m));
		strcpy(m.from, "phil"); strcpy(m.to, "anna@beta"); strcpy(m.subject, "Hi");
		mark = nout;
		CK(fed_mail(&m, "hello", 5) == 1, "a letter to anna@beta: on its way");
		steps(2);
		to_fed(mark, buf, sizeof(buf), 3);
		CK(strstr(buf, "MAIL beta@t bbs.mail ") && strstr(buf, "\"to\":\"anna\"") && strstr(buf, "\"from\":\"phil\""),
			"MAIL beta@t -- the name as this network gives it; to anna, from phil");
		int before = msg_count(0);
		deliver_data(FED, 3, "ERR no mail key for beta yet (its node info has not arrived)\n");
		steps(2);
		msg_t n2; idx_t e;
		static char body[512];
		CK(msg_count(0) == before + 1 && msg_head(0, before + 1, &n2, &e) && !strcmp(n2.to, "phil") &&
			!strcmp(n2.from, "postmaster") && !strcmp(n2.subject, "Not sent: Hi"), "refused: a notice in phil's mailbox");
		uint32_t bl = msg_body(0, &n2, body, sizeof(body));
		body[bl] = 0;
		CK(strstr(body, "anna@beta") && strstr(body, "no mail key for beta yet"), "saying to whom, and why");
		mark = nout;
		fed_mail(&m, "again", 5);
		steps(2);
		to_fed(mark, buf, sizeof(buf), 3);
		CK(strstr(buf, "MAIL beta") && !strstr(buf, "\"body\":\"hello\""), "and it is out of the outbox: not sent again");
		deliver_data(FED, 3, "OK 2222 10\n");			// that second letter, taken
		steps(2);

		// -- 8. a post too large for its network: said to its writer --
		msg_t p;
		memset(&p, 0, sizeof(p));
		strcpy(p.from, "phil"); strcpy(p.subject, "Long one");
		mark = nout;
		CK(fed_post(1, &p, "a long post", 11) == 1, "a post: on its way");
		steps(2);
		before = msg_count(0);
		deliver_data(FED, 3, "ERR 1900 bytes: larger than this network takes (1024)\n");
		steps(2);
		CK(msg_count(0) == before + 1 && msg_head(0, before + 1, &n2, &e) && !strcmp(n2.to, "phil") &&
			!strcmp(n2.subject, "Not posted: Long one"), "refused: a notice in phil's mailbox, 'Not posted'");
		bl = msg_body(0, &n2, body, sizeof(body));
		body[bl] = 0;
		CK(strstr(body, "General") && strstr(body, "larger than this network takes (1024)"), "naming the forum, and why");
	}

	// -- 9. fed restarted on the SAME pid: it looks alive -- its silence tells --
	{
		char key[200];
		snprintf(key, sizeof(key), "OK %s 0123456789abcdef alpha\n", self);
		// a fresh link, nothing outstanding: fed closes, the BBS comes back,
		// its KEY and SUB answered
		deliver(FED, Z_PORT_CLOSE, 3, z_obj_none());
		steps(2);
		ticks += 6 * Z_TICK_HZ;
		steps(2);
		deliver(FED, Z_PORT_CONNECTED, 0, z_obj_uint32(4));
		steps(2);
		char hello[260];
		snprintf(hello, sizeof(hello), "%sOK subscribed after 9\n", key);
		deliver_data(FED, 4, hello);
		steps(2);
		mark = nout;
		ticks += 61 * Z_TICK_HZ;
		steps(2);
		to_fed(mark, buf, sizeof(buf), 4);
		CK(strstr(buf, "KEY\n") != NULL && find(FED, Z_PORT_CONNECT, mark) < 0,
			"a minute silent, nothing outstanding: a harmless KEY -- is fed there? -- and no reset");
		deliver_data(FED, 4, key);			// a live fed answers
		steps(2);
		mark = nout;
		ticks += 30 * Z_TICK_HZ;
		steps(2);
		CK(find(FED, Z_PORT_CONNECT, mark) < 0, "answered: the link stays");
		// now nobody answers -- a new fed on the old pid, which the pid check cannot tell
		ticks += 31 * Z_TICK_HZ;
		steps(2);							// the next KEY goes...
		mark = nout;
		ticks += 61 * Z_TICK_HZ;
		steps(2);							// ...unanswered for a minute: reset
		ticks += 6 * Z_TICK_HZ;
		steps(2);							// five seconds on: connecting again
		CK(find(FED, Z_PORT_CONNECT, mark) >= 0, "unanswered for a minute: the link reset, and a CONNECT to whoever fed0 is now");
		mark = nout;
		deliver(FED, Z_PORT_CONNECTED, 0, z_obj_uint32(5));
		steps(2);
		to_fed(mark, buf, sizeof(buf), 5);
		CK(strstr(buf, "SUB bbs t/*") != NULL, "and the new link subscribes again");
	}

	// -- 10. a forum given a topic later: its history, once --
	{
		char key[200], hello[260], p[160], topics[400];
		snprintf(key, sizeof(key), "OK %s 0123456789abcdef alpha\n", self);
		snprintf(hello, sizeof(hello), "%sOK subscribed after 20\n", key);
		deliver_data(FED, 5, hello);
		steps(2);
		// the file, since the first connection: the topics it had, and no history asked
		snprintf(p, sizeof(p), "%s/fed-topics", dir);
		FILE *f = fopen(p, "r");
		size_t tl = f ? fread(topics, 1, sizeof(topics) - 1, f) : 0;
		if (f) fclose(f);
		topics[tl] = 0;
		CK(!strcmp(topics, "t/forum/general\n"), "fed-topics from the first connection: general's topic -- its history not asked for, it had it");
		// a forum added, with a topic never carried
		snprintf(p, sizeof(p), "%s/forums.cfg", dir);
		f = fopen(p, "w");
		fputs("general; General; 0; 10; Anything; t/forum/general\nnews; News; 0; 10; Also far; t/forum/news\n", f);
		fclose(f);
		areas_load();
		int news = area_find("news");
		deliver(FED, Z_PORT_CLOSE, 5, z_obj_none());
		steps(2);
		ticks += 6 * Z_TICK_HZ;
		steps(2);
		mark = nout;
		deliver(FED, Z_PORT_CONNECTED, 0, z_obj_uint32(6));
		steps(2);
		to_fed(mark, buf, sizeof(buf), 6);
		CK(strstr(buf, "SUB bbs t/* t/forum/news\n") != NULL, "reconnected: SUB with t/forum/news's history (%s)", buf);
		// its history: an older post on it, then HIST END
		char payload[300], obj[1200], line[40];
		int pl = snprintf(payload, sizeof(payload), "{\"from\":\"dave\",\"subject\":\"Old news\",\"date\":%u,\"body\":\"from before\"}",
			(unsigned)plat_now());
		int ol = snprintf(obj, sizeof(obj), "ZFED1\ntopic: t/forum/news\ntype: bbs.post\nformat: json\nkind: log\n"
			"origin: %s\ntime: %u\nseq: 2\nlen: %d\n\n%s\nsig: %0128d\n", other, (unsigned)plat_now(), pl, payload, 0);
		deliver_data(FED, 6, hello);
		steps(1);
		int ln = snprintf(line, sizeof(line), "OBJ 3 %d\n", ol);
		static char whole[1400];
		memcpy(whole, line, (size_t)ln);
		memcpy(whole + ln, obj, (size_t)ol);
		memcpy(whole + ln + ol, "HIST END\n", 10);
		deliver_data(FED, 6, whole);
		steps(3);
		CK(news > 0 && msg_count(news) == 1, "its history: the older post, in News");
		f = fopen(p, "r");
		(void)f; if (f) fclose(f);
		snprintf(p, sizeof(p), "%s/fed-topics", dir);
		f = fopen(p, "r");
		tl = f ? fread(topics, 1, sizeof(topics) - 1, f) : 0;
		if (f) fclose(f);
		topics[tl] = 0;
		CK(strstr(topics, "t/forum/news\n") != NULL, "after HIST END: t/forum/news recorded as carried");
		// the next connection asks for no history
		deliver(FED, Z_PORT_CLOSE, 6, z_obj_none());
		steps(2);
		ticks += 6 * Z_TICK_HZ;
		steps(2);
		mark = nout;
		deliver(FED, Z_PORT_CONNECTED, 0, z_obj_uint32(7));
		steps(2);
		to_fed(mark, buf, sizeof(buf), 7);
		CK(strstr(buf, "SUB bbs t/*\n") != NULL, "the next connection: a plain SUB -- never the history again (%s)", buf);
		// an older fed, which knows no history: subscribed without it
		snprintf(p, sizeof(p), "%s/forums.cfg", dir);
		f = fopen(p, "w");
		fputs("general; General; 0; 10; Anything; t/forum/general\nnews; News; 0; 10; Also far; t/forum/news\n"
			"old; Old; 0; 10; X; t/forum/old\n", f);
		fclose(f);
		areas_load();
		deliver_data(FED, 7, hello);
		steps(2);
		deliver(FED, Z_PORT_CLOSE, 7, z_obj_none());
		steps(2);
		ticks += 6 * Z_TICK_HZ;
		steps(2);
		deliver(FED, Z_PORT_CONNECTED, 0, z_obj_uint32(8));
		steps(2);
		mark = nout;
		char refused[260];
		snprintf(refused, sizeof(refused), "%sERR ?\n", key);
		deliver_data(FED, 8, refused);
		steps(2);
		to_fed(mark, buf, sizeof(buf), 8);
		CK(strstr(buf, "SUB bbs t/*\n") != NULL, "an older fed refusing the history: subscribed without it (%s)", buf);
	}

	printf("bbs fed-link test: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
