#ifndef FW_H
#define FW_H
/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * netx's firewall (fw.c, docs/netx.md "The firewall"). Built with
 * -DNETX only.
 *
 * Two places a packet is judged: on its way to the board (fw_input())
 * and on its way through (fw_forward()). The rules in
 * /data/net/firewall.cfg come first, in order, the first that matches
 * deciding; then the defaults, which only ever stop what comes in by a
 * WAN interface (one named wan, or with NAT):
 *
 *   to the board   ping and ICMP errors; TCP to the services in
 *                  <if>.allow (a SYN to anything else is dropped;
 *                  the rest of TCP is the stack's to answer); UDP that
 *                  answers what the board itself sent (fw_note()), or
 *                  to a UDP service in <if>.allow
 *   through        nothing, except what NAT sent on (a connection the
 *                  other side opened, or a port forward), which is not
 *                  judged here
 *
 * Everything else passes: a board with no WAN interface and no rules
 * filters nothing.
 */

#include <stdint.h>
#include <stdbool.h>

#define FW_RULES	64
#define FW_ALLOW	16		/* services per interface */

typedef struct {
	uint8_t allow;			/* 1 allow, 0 deny */
	int8_t in, out;			/* interface index, -1 any */
	int8_t to_board;		/* 1 to the board only, 0 through only, -1 both */
	uint8_t proto;			/* 0 any */
	uint32_t src, src_mask, dst, dst_mask;
	uint16_t sport_lo, sport_hi, dport_lo, dport_hi;	/* 0-65535: any */
	uint32_t hits;
} fw_rule_t;

typedef struct {
	uint8_t proto;			/* 6, 17, or 1 for ping */
	uint16_t port;
} fw_service_t;

// Read the rules (Z_PATH_NET_FIREWALL), once the interfaces are known:
// 0, or -1 with the line that is wrong said on the console.
int fw_load(void);
// The same from text (the tests).
int fw_parse(char *text);

// A service an interface lets in: "ssh", "telnet", "http", "https",
// "ping", "N" (TCP) or "udp/N". false: not one of these.
bool fw_allow(int ifx, const char *what);

// Judged on the way to the board (p: the IP packet, len bytes) from the
// current interface; true: let it in.
bool fw_input(const uint8_t *p, uint16_t len);
// Judged on the way through, from interface in to out.
bool fw_forward(const uint8_t *p, uint16_t len, int in, int out);
// What the board sends (p: the IP packet built), for the replies UDP
// may let in.
void fw_note(const uint8_t *p);

int fw_nrules(void);
const fw_rule_t *fw_rule(int i);
extern uint32_t fw_dropped_in, fw_dropped_fwd;

#endif
