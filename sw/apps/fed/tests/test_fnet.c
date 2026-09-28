/*
 * Host test for sw/apps/fed/core/fnet.c: node lists. docs/fed.md,
 * "Networks". Every kind of wrong list is refused WHOLE; the right ones
 * give the right members.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/fnet.h"

static int checks, fails;
#define CK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define KA "1111111111111111111111111111111111111111111111111111111111111111"
#define KB "2222222222222222222222222222222222222222222222222222222222222222"

static fnet_list_t L;
static char err[160];

static int parse(const char *net, const char *js) {
	return fnet_parse(net, (const uint8_t *)js, (uint32_t)strlen(js), &L, err, sizeof(err));
}

int main(void) {
	uint8_t ka[32], kb[32], kc[32];
	const char *addr;
	memset(ka, 0x11, 32); memset(kb, 0x22, 32); memset(kc, 0x33, 32);

	CK(parse("timeless", "{\"network\":\"timeless\",\"nodes\":[{\"key\":\"" KA "\",\"name\":\"machdyne\",\"sysop\":\"phil\","
		"\"addr\":\"bbs.machdyne.com:9070\"},{\"key\":\"" KB "\"}]}") == 0, "a list: %s", err);
	CK(L.n == 2 && fnet_has(&L, ka, &addr) && !strcmp(addr, "bbs.machdyne.com:9070") && fnet_has(&L, kb, &addr) && !addr[0] &&
		!fnet_has(&L, kc, NULL), "its members, and their addresses");
	CK(parse("timeless", "{\"network\":\"timeless\",\"nodes\":[]}") == 0 && L.n == 0, "an empty list is a list");
	CK(parse("timeless", "{\"nodes\":[{\"key\":\"" KA "\",\"colour\":\"blue\"}],\"network\":\"timeless\",\"motd\":\"hi\"}") == 0 &&
		L.n == 1, "unknown fields ignored, in any order: %s", err);
	CK(parse("timeless", "{\"network\":\"timeless\",\"nodes\":[{\"key\":\"" KA "\",\"addr\":\"10.0.0.5:9070\"}]}") == 0, "an IPv4 address");

	// moderators: who may cancel others' objects, and where
	{
		CK(parse("timeless", "{\"network\":\"timeless\",\"nodes\":[{\"key\":\"" KA "\"}],\"moderators\":["
			"{\"key\":\"" KB "\",\"topics\":[\"timeless/forum/*\",\"timeless/news\"]}]}") == 0 && L.nmods == 1,
			"a list with a moderator");
		CK(fnet_moderates(&L, kb, "timeless/forum/general") && fnet_moderates(&L, kb, "timeless/news") &&
			!fnet_moderates(&L, kb, "timeless/news/x") && !fnet_moderates(&L, kb, "timeless/forum") &&
			!fnet_moderates(&L, kb, "timeless/mail/1234") && !fnet_moderates(&L, ka, "timeless/forum/general"),
			"moderating: below a /* pattern, a literal exactly -- nothing else, and nobody else");
	}
	// a profile: the network's limit and its cryptography
	CK(parse("timeless", "{\"network\":\"timeless\",\"nodes\":[],\"profile\":{\"max_object\":1024,\"suite\":\"classical\"}}") == 0 &&
		L.max_object == 1024 && L.classical, "a profile: 1024 bytes, classical");
	CK(parse("timeless", "{\"network\":\"timeless\",\"nodes\":[]}") == 0 && L.max_object == 0 && !L.classical,
		"no profile: no limit but the protocol's, hybrid");
	struct { const char *js, *why; } bad[] = {
		{ "{\"network\":\"timeless\",\"nodes\":[],\"profile\":[]}", "a profile not an object" },
		{ "{\"network\":\"timeless\",\"nodes\":[],\"profile\":{\"max_object\":511}}", "a limit below 512" },
		{ "{\"network\":\"timeless\",\"nodes\":[],\"profile\":{\"max_object\":99999}}", "a limit above the largest object" },
		{ "{\"network\":\"timeless\",\"nodes\":[],\"profile\":{\"max_object\":\"1024\"}}", "a limit that is not a number" },
		{ "{\"network\":\"timeless\",\"nodes\":[],\"profile\":{\"suite\":\"fast\"}}", "a suite that is neither" },
		{ "{\"network\":\"timeless\",\"nodes\":[{\"key\":\"" KA "\",\"name\":\"alpha\"},{\"key\":\"" KB "\",\"name\":\"alpha\"}]}",
			"two nodes with one name (phil@alpha would be either)" },
		{ "{\"network\":\"timeless\",\"nodes\":[],\"moderators\":{}}", "moderators not an array" },
		{ "{\"network\":\"timeless\",\"nodes\":[],\"moderators\":[1]}", "a moderator not an object" },
		{ "{\"network\":\"timeless\",\"nodes\":[],\"moderators\":[{\"topics\":[\"timeless/x\"]}]}", "a moderator without a key" },
		{ "{\"network\":\"timeless\",\"nodes\":[],\"moderators\":[{\"key\":\"" KA "\"}]}", "a moderator without topics" },
		{ "{\"network\":\"timeless\",\"nodes\":[],\"moderators\":[{\"key\":\"" KA "\",\"topics\":[]}]}", "no topics" },
		{ "{\"network\":\"timeless\",\"nodes\":[],\"moderators\":[{\"key\":\"" KA "\",\"topics\":[\"other/forum/*\"]}]}",
			"a moderator of another network's topics" },
		{ "{\"network\":\"timeless\",\"nodes\":[],\"moderators\":[{\"key\":\"" KA "\",\"topics\":[\"timeless/*/x\"]}]}",
			"a * not at the end" },
		{ "{\"network\":\"timeless\",\"nodes\":[],\"moderators\":[{\"key\":\"" KA "\",\"topics\":[\"timeless/\"]}]}",
			"a pattern ending in /" },
		{ "{\"network\":\"timeless\",\"nodes\":[],\"moderators\":[{\"key\":\"" KA "\",\"topics\":[\"timeless/a\",\"timeless/b\","
			"\"timeless/c\",\"timeless/d\",\"timeless/e\",\"timeless/f\",\"timeless/g\",\"timeless/h\",\"timeless/i\"]}]}", "nine patterns" },
		{ "{\"network\":\"other\",\"nodes\":[]}", "another network's list" },
		{ "{\"nodes\":[]}", "no network" },
		{ "{\"network\":\"timeless\"}", "no nodes" },
		{ "{\"network\":\"timeless\",\"nodes\":{}}", "nodes not an array" },
		{ "[1,2]", "not an object" },
		{ "{\"network\":\"timeless\",\"nodes\":[1]}", "a node not an object" },
		{ "{\"network\":\"timeless\",\"nodes\":[{\"name\":\"x\"}]}", "a node without a key" },
		{ "{\"network\":\"timeless\",\"nodes\":[{\"key\":\"" KA "\"},{\"key\":\"" KA "\"}]}", "the same key twice" },
		{ "{\"network\":\"timeless\",\"nodes\":[{\"key\":\"ABCD111111111111111111111111111111111111111111111111111111111111\"}]}", "an uppercase key" },
		{ "{\"network\":\"timeless\",\"nodes\":[{\"key\":\"1111\"}]}", "a short key" },
		{ "{\"network\":\"timeless\",\"nodes\":[{\"key\":5}]}", "a key that is not a string" },
		{ "{\"network\":\"timeless\",\"nodes\":[{\"key\":\"" KA "\",\"name\":\"Machdyne\"}]}", "a name with a capital" },
		{ "{\"network\":\"timeless\",\"nodes\":[{\"key\":\"" KA "\",\"name\":\"\"}]}", "an empty name" },
		{ "{\"network\":\"timeless\",\"nodes\":[{\"key\":\"" KA "\",\"name\":\"abcdefghijklmnopqrstuvwxyz0123456\"}]}", "a 33-byte name" },
		{ "{\"network\":\"timeless\",\"nodes\":[{\"key\":\"" KA "\",\"sysop\":\"a\\nb\"}]}", "a newline in a sysop" },
		{ "{\"network\":\"timeless\",\"nodes\":[{\"key\":\"" KA "\",\"sysop\":\"x\\u0007\"}]}", "a bell in a sysop" },
		{ "{\"network\":\"timeless\",\"nodes\":[{\"key\":\"" KA "\",\"sysop\":\"\"}]}", "an empty sysop" },
		{ "{\"network\":\"timeless\",\"nodes\":[{\"key\":\"" KA "\",\"addr\":\"bbs.machdyne.com\"}]}", "an address without a port" },
		{ "{\"network\":\"timeless\",\"nodes\":[{\"key\":\"" KA "\",\"addr\":\"host:0\"}]}", "port 0" },
		{ "{\"network\":\"timeless\",\"nodes\":[{\"key\":\"" KA "\",\"addr\":\"host:70000\"}]}", "port 70000" },
		{ "{\"network\":\"timeless\",\"nodes\":[{\"key\":\"" KA "\",\"addr\":\"host:090\"}]}", "a port with a leading zero" },
		{ "{\"network\":\"timeless\",\"nodes\":[{\"key\":\"" KA "\",\"addr\":\"ho st:90\"}]}", "a space in a host" },
		{ "{\"network\":\"timeless\",\"nodes\":[{\"key\":\"" KA "\",\"addr\":\":90\"}]}", "no host" },
		{ "{\"network\":\"timeless\",\"nodes\":[{\"key\":\"" KA "\"},]}", "not strict JSON" },
		{ "{\"network\":\"timeless\",\"network\":\"timeless\",\"nodes\":[]}", "a duplicate field" },
	};
	int right = 0, n = (int)(sizeof(bad) / sizeof(bad[0]));
	for (int i = 0; i < n; i++) {
		if (parse("timeless", bad[i].js) == -1) right++;
		else printf("  accepted: %s\n", bad[i].why);
	}
	CK(right == n, "%d wrong lists, %d refused", n, right);

	// the limit: 200 nodes a list, not 201
	{
		static char js[40000];
		for (int count = 200; count <= 201; count++) {
			int o = sprintf(js, "{\"network\":\"t\",\"nodes\":[");
			for (int i = 0; i < count; i++)
				o += sprintf(js + o, "%s{\"key\":\"%064x\"}", i ? "," : "", i + 1);
			sprintf(js + o, "]}");
			int r = parse("t", js);
			CK(count == 200 ? (r == 0 && L.n == 200) : r == -1, "%d nodes: %s", count, count == 200 ? "taken" : "refused");
		}
	}
	CK(fnet_name_ok("timeless") && fnet_name_ok("a-1") && !fnet_name_ok("") && !fnet_name_ok("a/b") && !fnet_name_ok("A"), "network names");

	printf("fnet: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
