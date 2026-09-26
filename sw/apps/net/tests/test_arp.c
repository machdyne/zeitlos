/*
 * Host test for arp.c: what the cache learns, and what it keeps.
 *
 *   make test
 *
 * The case that matters: a busy network's broadcast ARP chatter must
 * not push out the hosts this machine is actually talking to. It used
 * to -- every ARP sender was learned, and a full cache always reused
 * slot 0 -- and tget failed with "arp not resolved yet" on every retry.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

uint8_t eth_our_mac[6] = { 2, 0, 0, 0, 0, 1 };
static int sent;
bool eth_send(const uint8_t dst[6], uint16_t type, const uint8_t *p, uint16_t len) {
	(void)dst; (void)type; (void)p; (void)len;
	sent++;
	return true;
}

#include "../arp.c"

#define US 0xC0A8B29Au              /* 192.168.178.154 */
static int checks, fails;
#define CK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %d: ", __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

/* An ARP packet from `ip` (MAC derived from it) asking for or answering `target`. */
static void arp_in(uint32_t ip, uint32_t target, uint16_t oper) {
	uint8_t p[28];
	memset(p, 0, sizeof(p));
	p[1] = 1; p[2] = 8; p[4] = 6; p[5] = 4; p[7] = (uint8_t)oper;
	p[8] = 0xaa; p[9] = 0xbb; p[10] = (uint8_t)(ip >> 24); p[11] = (uint8_t)(ip >> 16);
	p[12] = (uint8_t)(ip >> 8); p[13] = (uint8_t)ip;
	p[14] = (uint8_t)(ip >> 24); p[15] = (uint8_t)(ip >> 16); p[16] = (uint8_t)(ip >> 8); p[17] = (uint8_t)ip;
	p[24] = (uint8_t)(target >> 24); p[25] = (uint8_t)(target >> 16);
	p[26] = (uint8_t)(target >> 8); p[27] = (uint8_t)target;
	arp_handle(p + 8, p, sizeof(p));
}

static bool known(uint32_t ip) {
	uint8_t m[6];
	return arp_lookup(ip, m);
}

int main(void) {
	arp_init(US);

	/* broadcasts between other hosts teach us nothing */
	for (uint32_t i = 0; i < 50; i++) arp_in(0xC0A8B200u + 10 + i, 0xC0A8B200u + 70 + i, 1);
	CK(!known(0xC0A8B200u + 10), "another host's who-has is not learned");

	/* a request for us, and a reply to ours, are */
	arp_in(0xC0A8B201u, US, 1);                       /* the router asks for us */
	CK(known(0xC0A8B201u), "a request for our address teaches its sender");
	arp_request(0xC0A8B266u);
	arp_in(0xC0A8B266u, US, 2);                       /* loom answers */
	CK(known(0xC0A8B266u), "a reply to our request is learned");

	/* and chatter afterwards does not push it out */
	for (uint32_t i = 0; i < 200; i++) arp_in(0xC0A8B200u + (i % 60) + 100, 0xC0A8B200u + 5, 1);
	CK(known(0xC0A8B266u) && known(0xC0A8B201u), "200 broadcasts later, both still known");

	/* a known host's broadcast refreshes its entry (a new MAC) */
	{
		uint8_t m[6];
		arp_in(0xC0A8B266u, 0xC0A8B200u + 9, 1);      /* loom asks someone else */
		arp_lookup(0xC0A8B266u, m);
		CK(m[0] == 0xaa, "a known host's broadcast still refreshes it");
	}

	/* full: the least recently used goes, not slot 0 */
	for (uint32_t i = 0; i < ARP_CACHE_SIZE; i++) arp_in(0xC0A8B200u + 150 + i, US, 1);
	{
		uint8_t m[6];
		arp_lookup(0xC0A8B200u + 150, m);             /* keep this one in use */
		arp_in(0xC0A8B200u + 199, US, 1);             /* one more than fits */
		CK(known(0xC0A8B200u + 150), "a recently used entry survives");
		CK(!known(0xC0A8B200u + 151), "the least recently used one is evicted");
		CK(known(0xC0A8B200u + 199), "the new one is in");
	}

	printf("arp: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
