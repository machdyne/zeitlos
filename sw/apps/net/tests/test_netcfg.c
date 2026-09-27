/*
 * netcfg_parse_ipv4() against what sscanf("%u.%u.%u.%u%c") -- the call
 * it replaced -- accepts and rejects, so a config file that worked
 * before still works (docs/networking.md, "Why net does not call
 * sscanf"). Both are run on every case and must agree; the build
 * machine's sscanf stands in for newlib's, which reads %u the same way.
 *
 *   make -C sw/apps/net test
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

int netcfg_parse_ipv4(const char *s, uint32_t *out);

// netcfg_load() reads the card; nothing here calls it.
char *fs_mallocfile(char *filename) { (void)filename; return NULL; }

static int old_parse(const char *s, uint32_t *out) {
	unsigned a, b, c, d;
	char extra;
	if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4) return -1;
	if (a > 255 || b > 255 || c > 255 || d > 255) return -1;
	*out = (a << 24) | (b << 16) | (c << 8) | d;
	return 0;
}

int main(void) {
	static const char *cases[] = {
		"192.168.1.10", "0.0.0.0", "255.255.255.255", "10.0.0.1",
		"010.000.000.001", "0000192.168.001.001", " 192.168.1.1",
		"192. 168.1.1", "192.168.1.1 ", "192.168.1.1x", "192.168.1",
		"192.168.1.1.5", "256.1.1.1", "1.1.1.256", "999999999999.1.1.1",
		"", ".", "...", "1..1.1", "a.b.c.d", "1.2.3.", ".1.2.3",
		"192.168.1.10\t", "\t10.0.0.1",
	};
	int fails = 0, n = (int)(sizeof(cases) / sizeof(cases[0]));
	for (int i = 0; i < n; i++) {
		uint32_t a = 0xdeadbeef, b = 0xdeadbeef;
		int ra = old_parse(cases[i], &a), rb = netcfg_parse_ipv4(cases[i], &b);
		if (ra != rb || (ra == 0 && a != b)) {
			printf("FAIL [%s]: sscanf %d %08x, parser %d %08x\n", cases[i], ra, a, rb, b);
			fails++;
		}
	}

	// The one deliberate difference. %u wraps a number too big for an
	// unsigned int, so sscanf read "4294967297" as 1 and accepted this
	// as 1.0.0.1 -- a config typo that became a different address. The
	// parser refuses it.
	{
		uint32_t a = 0, b = 0;
		if (old_parse("4294967297.0.0.1", &a) != 0 || a != 0x01000001) {
			printf("NOTE: the build machine's sscanf no longer wraps %%u\n");
		}
		if (netcfg_parse_ipv4("4294967297.0.0.1", &b) != -1) {
			printf("FAIL: an overflowing number was accepted\n");
			fails++;
		}
	}

	printf("test_netcfg: %d cases agree with sscanf, %d do not; "
		"overflow refused\n", n - fails, fails);
	return fails ? 1 : 0;
}
