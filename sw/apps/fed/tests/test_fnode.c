/*
 * Host test for sw/apps/fed/core/fnode.c: the configuration and the
 * rate limits. (Sessions and the local interface: test_fsess.c and
 * live_fed.py.)
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../core/fnode.h"
#include "../core/fstore.h"
#include "../core/fobj.h"
#include "../../../ext/monocypher/monocypher-ed25519.h"

// A signed node list from `sk` on t/nodes, its payload `js`, into the store.
static void put_list_on(const char *topic, const uint8_t sk[64], const char *js, uint64_t t) {
	static uint8_t buf[FOBJ_MAX];
	fobj_t o;
	uint32_t pos;
	memset(&o, 0, sizeof(o));
	snprintf(o.topic, sizeof(o.topic), "%s", topic);
	snprintf(o.type, sizeof(o.type), "fed.nodes");
	snprintf(o.key, sizeof(o.key), "list");
	o.format = FOBJ_JSON; o.kind = FOBJ_STATE; o.time = t; o.seq = t; o.len = (uint32_t)strlen(js);
	int n = fobj_make(&o, (const uint8_t *)js, sk, buf, sizeof(buf));
	fstore_put(buf, (uint32_t)n, 0, &pos);
}
static void put_list(const uint8_t sk[64], const char *js, uint64_t t) { put_list_on("t/nodes", sk, js, t); }

static bool fake_resolve(const char *host, char *ip, int cap) {
	if (!strcmp(host, "localhost")) { snprintf(ip, (size_t)cap, "127.0.0.2"); return true; }
	if (!strcmp(host, "127.0.0.1")) { snprintf(ip, (size_t)cap, "127.0.0.1"); return true; }
	return false;
}

static void hex(const uint8_t *k, char *out) { for (int i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", k[i]); }

extern int plat_quiet;
static int checks, fails;
#define CK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define K1 "6a1f000000000000000000000000000000000000000000000000000000000001"
#define K2 "91d2000000000000000000000000000000000000000000000000000000000002"

int main(void) {
	fcfg_t c;
	char err[120];
	plat_quiet = 1;

	// -- the configuration --
	CK(fnode_config("name: machdyne   # a comment\nlisten: 9070\npoll_minutes: 15\n"
		"peer: " K1 " bbs.machdyne.com:9070\npeer: " K2 " trusted-relay\n"
		"subscribe: timeless/forum/*; timeless/nodes\nretain: 90d; timeless/door/*=30d\n"
		"network: timeless " K1 "\nblock: " K2 "\n", &c, err, sizeof(err)), "the example from docs/fed.md: %s", err);
	CK(!strcmp(c.name, "machdyne") && c.listen == 9070 && c.poll_s == 900, "name, listen, poll");
	CK(c.npeers == 2 && !strcmp(c.peers[0].host, "bbs.machdyne.com") && c.peers[0].port == 9070 && !c.peers[0].trusted,
		"an outbound peer");
	CK(!c.peers[1].host[0] && c.peers[1].trusted, "an inbound, trusted-relay peer");
	CK(!strcmp(c.wants, "timeless/forum/*\ntimeless/nodes"), "subscribe: the patterns");
	CK(c.retain_default == 90 * 86400 && c.nretain == 1 && c.retain[0].secs == 30 * 86400 &&
		!strcmp(c.retain[0].pat, "timeless/door/*"), "retain: the default and a pattern");
	CK(c.nblocked == 1, "block");
	CK(c.nnetworks == 1 && !strcmp(c.networks[0].name, "timeless") && c.networks[0].publisher[0] == 0x6a,
		"network: its name and its publisher's key");
	CK(!fnode_config("network: Time " K1 "\n", &c, err, sizeof(err)), "a network name with capitals: refused");
	CK(!fnode_config("network: timeless\n", &c, err, sizeof(err)), "a network without its publisher's key: refused");
	CK(!fnode_config("network: timeless " K1 "\nnetwork: timeless " K2 "\n", &c, err, sizeof(err)) && strstr(err, "line 2") &&
		strstr(err, "here already"), "two networks with one name: refused -- they would share topics (%s)", err);
	CK(!fnode_config("peer: 12345\n", &c, err, sizeof(err)) && strstr(err, "line 1"), "a short key: refused, with the line (%s)", err);
	CK(!fnode_config("name: x\npeer: " K1 " host\n", &c, err, sizeof(err)) && strstr(err, "line 2"), "host without a port (%s)", err);
	CK(!fnode_config("colour: blue\n", &c, err, sizeof(err)) && strstr(err, "unknown"), "an unknown key (%s)", err);
	CK(!fnode_config("retain: forever\n", &c, err, sizeof(err)), "a duration that is not one");
	CK(fnode_config("radio: port=301 channel=2 max=1500 pace=3000\n", &c, err, sizeof(err)) && c.radio &&
		c.radio_port == 301 && c.radio_channel == 2 && c.radio_max == 1500 && c.radio_pace_ms == 3000, "a radio link (%s)", err);
	CK(!fnode_config("radio: port=255\n", &c, err, sizeof(err)) && !fnode_config("radio: max=9000\n", &c, err, sizeof(err)) &&
		!fnode_config("radio: fast\n", &c, err, sizeof(err)), "a radio: a Meshtastic port, over 4 KB, not key=value -- refused");

	// -- rate limits: each failure from an address doubles its wait --
	{
		char dir[64], cmd[160];
		snprintf(dir, sizeof(dir), "/tmp/fnode-test-%d", (int)getpid());
		snprintf(cmd, sizeof(cmd), "mkdir -p %s && printf 'name: t\\nlisten: 1\\n' > %s/fed.cfg", dir, dir);
		if (system(cmd)) return 1;
		uint8_t seed[32] = { 1 };
		CK(fnode_start(dir, seed, err, sizeof(err)) == 0, "a node starts: %s", err);
		uint32_t t = 1000000;
		CK(fnode_admit("10.0.0.9", t), "a new address is admitted");
		for (int i = 1; i <= 3; i++) {
			fsess_t *s = fnode_session(false, -1, "10.0.0.9", t);
			fnode_session_end(s, t);			// ends before a handshake: a failure
		}
		// three failures: the next is 2^3 = 8 s away
		CK(!fnode_admit("10.0.0.9", t + 7000), "after three failures: refused 7 s later");
		CK(fnode_admit("10.0.0.9", t + 8000), "and admitted after 8 s");
		CK(fnode_admit("10.0.0.10", t + 1), "another address is not affected");
		fsess_t *a = fnode_session(false, -1, "x", t), *b = fnode_session(false, -1, "y", t), *d = fnode_session(false, -1, "z", t);
		CK(a && b && d && !fnode_session(false, -1, "w", t), "three sessions at most, then none");
		fnode_session_end(a, t); fnode_session_end(b, t); fnode_session_end(d, t);
		fnode_stop();
		snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
		if (system(cmd)) return 1;
	}
	// -- networks: membership from signed lists --
	{
		char dir[64], cmd[400], hp[65], hq[65], hz[65], hm[65], hw[65], hv[65], js[1200];
		uint8_t seed[32], psk[64], ppk[32], msk[64], mpk[32], qsk[64], qpk[32], zsk[64], zpk[32], wsk[64], wpk[32], vsk[64], vpk[32], x[32];
		memset(seed, 0x50, 32); crypto_ed25519_key_pair(psk, ppk, seed);		// the publisher
		memset(seed, 0x51, 32); crypto_ed25519_key_pair(msk, mpk, seed);		// a member, with an IP address
		memset(seed, 0x52, 32); crypto_ed25519_key_pair(qsk, qpk, seed);		// a member by the sysop's config
		memset(seed, 0x53, 32); crypto_ed25519_key_pair(zsk, zpk, seed);		// listed, but blocked here
		memset(seed, 0x54, 32); crypto_ed25519_key_pair(wsk, wpk, seed);		// listed with a host name
		memset(seed, 0x55, 32); crypto_ed25519_key_pair(vsk, vpk, seed);		// listed with a name that does not resolve
		memset(x, 0x77, 32);													// nobody
		hex(ppk, hp); hex(qpk, hq); hex(zpk, hz); hex(mpk, hm); hex(wpk, hw); hex(vpk, hv);
		snprintf(dir, sizeof(dir), "/tmp/fnode-net-%d", (int)getpid());
		snprintf(cmd, sizeof(cmd), "mkdir -p %s && printf 'name: n\\nnetwork: t %s\\nnetwork: u %s\\nmember: %s\\nblock: %s\\n' > %s/fed.cfg", dir, hp, hp, hq, hz, dir);
		if (system(cmd)) return 1;
		uint8_t s[32] = { 9 };
		fnode_set_resolver(fake_resolve);
		CK(fnode_start(dir, s, err, sizeof(err)) == 0, "a node in network t: %s", err);
		CK(!fnode_origin_ok(mpk, "t/forum"), "before any list: a stranger to t");
		snprintf(js, sizeof(js), "{\"network\":\"t\",\"nodes\":[{\"key\":\"%s\",\"addr\":\"127.0.0.1:1\"},{\"key\":\"%s\"},"
			"{\"key\":\"%s\",\"addr\":\"localhost:2\"},{\"key\":\"%s\",\"addr\":\"nowhere:3\"}]}", hm, hz, hw, hv);
		put_list(psk, js, 100);
		// a second network, u, whose only member is y -- not on t's list
		uint8_t ysk[64], ypk[32];
		char hy[65], ju[300];
		memset(seed, 0x56, 32); crypto_ed25519_key_pair(ysk, ypk, seed);
		hex(ypk, hy);
		snprintf(ju, sizeof(ju), "{\"network\":\"u\",\"nodes\":[{\"key\":\"%s\"}]}", hy);
		put_list_on("u/nodes", psk, ju, 100);
		CK(fnode_reload_lists() == 2, "both lists load");
		CK(fnode_origin_ok(ypk, "u/forum") && !fnode_origin_ok(ypk, "t/forum"),
			"a member of u only: on u's topics, not on t's");
		CK(fnode_origin_ok(ypk, "fed/other/9"), "and on fed/ topics, as a member of a network");
		CK(fnode_origin_ok(mpk, "t/forum") && fnode_origin_ok(mpk, "t/forum/sub"), "a member, on its network's topics");
		CK(!fnode_origin_ok(x, "t/forum"), "a stranger, not");
		CK(fnode_origin_ok(qpk, "t/forum"), "a member by the sysop's config, too");
		CK(fnode_origin_ok(mpk, "fed/other/1") && !fnode_origin_ok(x, "fed/other/1"), "fed/ topics: members of a network, only");
		{
			// a node's info (its mail keys): from that node, no other
			char msid[17], ysid[17], t1[40], t2[40];
			fobj_short_id(mpk, msid);
			fobj_short_id(ypk, ysid);
			snprintf(t1, sizeof(t1), "fed/node/%s", msid);
			snprintf(t2, sizeof(t2), "fed/node/%s", ysid);
			CK(fnode_origin_ok(mpk, t1) && !fnode_origin_ok(ypk, t1) && !fnode_origin_ok(mpk, t2) &&
				!fnode_origin_ok(mpk, "fed/node/1"), "fed/node/<short id>: its own node only -- not another member, not a made-up id");
		}
		CK(fnode_origin_ok(ppk, "t/nodes") && !fnode_origin_ok(mpk, "t/nodes"), "the list's topic: the publisher only");
		CK(!fnode_origin_ok(ppk, "t/forum"), "the publisher elsewhere: only as a member, and it is not one");
		CK(!fnode_origin_ok(zpk, "t/forum"), "listed, but blocked here: out");
		CK(!fnode_origin_ok(mpk, "fed/join"), "join requests: never stored");
		CK(fnode_connect_ok(mpk, "127.0.0.1") && !fnode_connect_ok(mpk, "10.0.0.9"), "the address check: from its listed address only");
		CK(fnode_connect_ok(wpk, "127.0.0.2") && !fnode_connect_ok(wpk, "127.0.0.1"), "a listed host name, resolved");
		CK(fnode_connect_ok(vpk, "1.2.3.4"), "a name that does not resolve: the address not checked, the key still is");
		CK(!fnode_connect_ok(x, "127.0.0.1") && fnode_connect_ok(qpk, "9.9.9.9"), "a stranger: no; a configured member: no address to check");
		// a broken newer list: refused whole, the previous stands
		snprintf(js, sizeof(js), "{\"network\":\"t\",\"nodes\":[{\"key\":\"%s\"},{\"key\":\"%s\"}]}", hw, hw);
		put_list(psk, js, 200);
		fnode_reload_lists();
		CK(fnode_origin_ok(mpk, "t/forum"), "a list with a duplicate key: refused, the previous one stands");
		// a list from someone else on t/nodes changes nothing
		snprintf(js, sizeof(js), "{\"network\":\"t\",\"nodes\":[]}");
		put_list(msk, js, 300);
		fnode_reload_lists();
		CK(fnode_origin_ok(mpk, "t/forum"), "a list from a key that is not the publisher's: ignored");
		// the next good list, without the member
		snprintf(js, sizeof(js), "{\"network\":\"t\",\"nodes\":[{\"key\":\"%s\"}]}", hw);
		put_list(psk, js, 400);
		fnode_reload_lists();
		CK(!fnode_origin_ok(mpk, "t/forum") && fnode_origin_ok(wpk, "t/forum"), "taken off the list: out");
		// this node may not publish what no node would take from it
		{
			int id = fnode_client_open();
			const uint8_t *p;
			const char *r1 = "PUB t/nodes fed.nodes json state list 2\n{}";
			fnode_client_input(id, (const uint8_t *)r1, (uint32_t)strlen(r1));
			uint32_t n = fnode_client_output(id, &p);
			CK(n > 3 && !memcmp(p, "ERR", 3), "PUB of a list it is not the publisher of: ERR");
			fnode_client_consumed(id, n);
			const char *rk = "KEY\n";
			fnode_client_input(id, (const uint8_t *)rk, (uint32_t)strlen(rk));
			n = fnode_client_output(id, &p);
			char want[80];
			snprintf(want, sizeof(want), "OK %02x", fnode_public_key()[0]);
			CK(n == 87 && !memcmp(p, want, 5) && p[67] == ' ' && p[84] == ' ' && !memcmp(p + n - 3, " n\n", 3),
				"KEY: this node's key, short id and name");
			fnode_client_consumed(id, n);
			const char *r2 = "PUB fed/join fed.join json log - 2\n{}";
			fnode_client_input(id, (const uint8_t *)r2, (uint32_t)strlen(r2));
			n = fnode_client_output(id, &p);
			CK(n > 3 && !memcmp(p, "ERR", 3), "PUB on fed/join: ERR");
			fnode_client_close(id);
		}
		// a person at a terminal (`port fed0` in term: Enter is \r)
		{
			static char got[4096];
			uint32_t gl = 0, n;
			const uint8_t *p;
			int id = fnode_client_open();
			#define TYPE(s) do { fnode_client_input(id, (const uint8_t *)(s), (uint32_t)strlen(s)); \
				while ((n = fnode_client_output(id, &p)) > 0 && gl + n < sizeof(got)) { memcpy(got + gl, p, n); gl += n; fnode_client_consumed(id, n); } \
				got[gl] = 0; } while (0)
			TYPE("K"); TYPE("E"); TYPE("Y"); TYPE("\r");
			CK(strstr(got, "KEY\r\n") && strstr(got, "OK ") && strstr(got, " n\r\n"),
				"a terminal: typing echoed, Enter a line end, the answer in \\r\\n lines");
			gl = 0;
			TYPE("KEYZ\x7f\r");
			CK(strstr(got, "KEYZ\b \b\r\n") && strstr(got, "OK "), "Backspace takes a typo back, and the command works");
			gl = 0;
			TYPE("KEY\r\n");
			CK(strstr(got, "KEY\r\nOK ") && !strstr(got, "ERR"), "a whole line pasted at once: echoed; \\r\\n one line end");
			fnode_client_close(id);
			// a program stays a program, even with a \r in a payload later
			id = fnode_client_open();
			gl = 0;
			TYPE("KEY\n");
			TYPE("PUB t/x t text log - 3\na\rb");
			CK(!strstr(got, "KEY\r\n") && strstr(got, " n\n") && !strstr(got, "\r\n"),
				"a program: no echo, \\n line ends -- a \\r in its payload changes nothing");
			fnode_client_close(id);
			#undef TYPE
		}
		fnode_stop();
		snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
		if (system(cmd)) return 1;
	}

	printf("fnode: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
