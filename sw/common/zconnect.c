/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Connection targets -- see sw/common/zconnect.h.
 *
 * Every function here was a command handler in sw/apps/repl/repl.c
 * before term needed the same thing. The comments came with them,
 * because the reasoning is about the protocols rather than about
 * repl.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "zconnect.h"
#include "zeitlos.h"
#include "zdns.h"		// z_resolve_host()
#include "znet.h"		// Z_NET_SSH_PREPARE
#include "zterm.h"		// Z_TERM_SET_PORT
#include "zport.h"

static const char *kind_words[] = { "port", "serial", "telnet", "ssh" };

bool z_conn_kind_from_word(const char *word, z_conn_kind_t *out) {
	int i;
	if (!word || !out) return false;
	for (i = 0; i < 4; i++) {
		if (!strcmp(word, kind_words[i])) {
			*out = (z_conn_kind_t)i;
			return true;
		}
	}
	return false;
}

const char *z_conn_kind_name(z_conn_kind_t kind) {
	if ((int)kind < 0 || (int)kind > 3) return "?";
	return kind_words[kind];
}

bool z_conn_kind_needs_text(z_conn_kind_t kind) {
	return kind != Z_CONN_SERIAL;
}

static void set_ip_detail(z_conn_target_t *t, const char *typed, uint32_t ip) {
	// Shows the resolved address alongside whatever was typed, which
	// makes it obvious which address a hostname actually went to.
	// Redundant when the text was already an IP, and harmless there.
	snprintf(t->detail, sizeof(t->detail), "%s (%lu.%lu.%lu.%lu)",
		typed,
		(unsigned long)((ip >> 24) & 0xff), (unsigned long)((ip >> 16) & 0xff),
		(unsigned long)((ip >> 8) & 0xff), (unsigned long)(ip & 0xff));
}

// -- port ------------------------------------------------------

static bool prep_port(const char *text, z_conn_target_t *t,
	char *err, size_t errlen) {

	if (!text || !*text) {
		snprintf(err, errlen,
			"usage: port <name>  (e.g. port portdemo0, port repl1)");
		return false;
	}

	if (strlen(text) >= sizeof(t->provider)) {
		snprintf(err, errlen, "port: name too long");
		return false;
	}

	// No lookup here, and no fixed-pid fallback. If the provider is
	// not running, the CONNECT simply fails at whoever tries it and
	// that terminal stays in local echo -- the same clean failure any
	// unreachable target gives. term's own startup connection to
	// "repl0" has a fallback pid; nothing typed by a user does.
	strcpy(t->provider, text);
	t->arg = z_obj_none();
	t->timeout_ticks = Z_CONN_TIMEOUT_LOCAL_TICKS;
	snprintf(t->detail, sizeof(t->detail), "%s", text);

	return true;

}

// -- serial ----------------------------------------------------

static bool prep_serial(const char *text, z_conn_target_t *t,
	char *err, size_t errlen) {

	uint32_t baud = 0;

	while (text && *text == ' ') text++;

	if (text && *text) {

		// Parsed strictly rather than with atoi(), because
		// atoi("fast") is 0 and 0 here means "keep the current rate"
		// -- a typo quietly doing something plausible instead of
		// saying so.
		const char *p = text;
		while (*p) {
			if (*p < '0' || *p > '9') {
				snprintf(err, errlen,
					"usage: serial [baud]  (e.g. serial 9600)");
				return false;
			}
			baud = baud * 10 + (uint32_t)(*p - '0');
			p++;
		}

		if (baud < 50 || baud > 3000000) {
			snprintf(err, errlen, "serial: %lu baud is outside 50..3000000",
				(unsigned long)baud);
			return false;
		}

	}

	strcpy(t->provider, "serial0");
	t->arg = z_obj_uint32(baud);
	t->timeout_ticks = Z_CONN_TIMEOUT_LOCAL_TICKS;

	if (baud)
		snprintf(t->detail, sizeof(t->detail), "UART1 at %lu baud",
			(unsigned long)baud);
	else
		snprintf(t->detail, sizeof(t->detail), "UART1");

	return true;

}

// -- telnet ----------------------------------------------------

static bool prep_telnet(const char *text, z_conn_target_t *t,
	char *err, size_t errlen) {

	uint32_t ip;
	char rerr[64];

	while (text && *text == ' ') text++;

	if (!text || !*text) {
		snprintf(err, errlen,
			"usage: telnet <ip-or-hostname>  "
			"(e.g. telnet 192.168.178.100, telnet myserver.local)");
		return false;
	}

	// z_resolve_host() (sw/common/zdns.h) tries a plain dotted-quad
	// parse first -- instant, no messaging -- and only falls back to
	// an actual DNS query via net's dns.c if that fails. THAT
	// FALLBACK BLOCKS for up to a few seconds in the worst case (no
	// response, net not running, or a genuine NXDOMAIN). See
	// zconnect.h's header on what that costs each caller.
	if (!z_resolve_host(text, &ip, rerr, sizeof(rerr))) {
		snprintf(err, errlen, "telnet: %s", rerr);
		return false;
	}

	// "net0" is net's own pidreg name. No fixed-pid fallback here,
	// matching the port kind above.
	strcpy(t->provider, "net0");
	t->arg = z_obj_uint32(ip);
	t->timeout_ticks = Z_CONN_TIMEOUT_NETWORK_TICKS;
	set_ip_detail(t, text, ip);

	return true;

}

// -- ssh -------------------------------------------------------

static bool prep_ssh(const char *text, z_conn_target_t *t,
	char *err, size_t errlen) {

	char user[32], host[80];
	const char *at;
	uint32_t ip, net_pid, token;
	char rerr[64];
	z_obj_t req;
	z_msg_t reply;
	z_obj_t *ok_obj, *err_obj, *tok_obj;

	while (text && *text == ' ') text++;

	if (!text || !*text) {
		snprintf(err, errlen,
			"usage: ssh [user@]<ip-or-hostname>  (e.g. ssh me@10.0.0.5)");
		return false;
	}

	// Split user@host. A missing username is not an error: net
	// prompts for one in band, the way PuTTY does, which is one less
	// thing to get wrong on a keyboard.
	user[0] = 0;
	at = strchr(text, '@');
	if (at) {
		size_t n = (size_t)(at - text);
		if (n >= sizeof(user)) {
			snprintf(err, errlen, "ssh: username too long");
			return false;
		}
		memcpy(user, text, n);
		user[n] = 0;
		text = at + 1;
	}

	if (strlen(text) >= sizeof(host)) {
		snprintf(err, errlen, "ssh: hostname too long");
		return false;
	}
	strcpy(host, text);

	// Same blocking resolve as telnet, with the same caveat.
	if (!z_resolve_host(host, &ip, rerr, sizeof(rerr))) {
		snprintf(err, errlen, "ssh: %s", rerr);
		return false;
	}

	// Step one of the two-step setup in sw/common/znet.h: hand `net`
	// the username DIRECTLY, in one hop, because forwarding a string
	// through Z_TERM_SET_PORT would have its pointer translated twice
	// and land on garbage. What comes back is a scalar token that CAN
	// be forwarded safely.
	//
	// By name, not the fixed Z_PID_NET, for the same reason zdns.c's
	// own lookup avoids it: if net is not running, a fixed pid sends
	// this into whatever else happens to occupy slot 2, and the call
	// hangs waiting for a reply that process will never send.
	if (!z_pid_lookup("net0", &net_pid)) {
		snprintf(err, errlen, "ssh: net is not running");
		return false;
	}

	req = z_obj_map(3);
	z_map_set(&req, "user", z_obj_str(user));
	z_map_set(&req, "ip", z_obj_uint32(ip));
	z_map_set(&req, "port", z_obj_uint32(22));
	z_msg_new_send(net_pid, Z_NET_SSH_PREPARE, 0, req);

	// BOUNDED wait, following zdns.c's own loop rather than
	// z_msg_wait(). That function spins FOREVER if the reply never
	// comes (zeitlos.c) -- and there is a very ordinary way for it
	// never to come: a `net` that predates Z_NET_SSH_PREPARE, or one
	// built with SSH_ENABLE=0, does not recognise the subject and
	// silently drops it. Since core apps live in the flash ZAR
	// archive (sw/os/zar.h), rebuilding one while running a stale
	// other is easy to do by accident.
	//
	// The symptom of getting this wrong is the worst kind. In repl,
	// where this code used to live, one mailbox serves every
	// connected term window -- so an unbounded wait froze all of them
	// at once, with no output anywhere. Found exactly that way.
	{
		uint32_t start = z_uptime_ticks();
		bool got = false;

		while ((z_uptime_ticks() - start) < (732u * 5)) {
			if (z_msg_read(&reply) != Z_OK) continue;
			if (reply.subject == Z_NET_SSH_PREPARE_REPLY) { got = true; break; }
			// anything else is discarded, same as zdns.c's own loop
			// and zport.c's connect wait
		}

		if (!got) {
			snprintf(err, errlen,
				"ssh: net (pid %lu) did not answer. Check the SERIAL "
				"console at boot for 'net: ssh support built in' -- if that "
				"line is absent, net is stale or built SSH_ENABLE=0; "
				"rebuild and reflash it.", (unsigned long)net_pid);
			return false;
		}
	}

	ok_obj = z_map_find(&reply.obj, "ok");
	if (!ok_obj || ok_obj->type != Z_UINT32 || !ok_obj->val.uint32) {
		err_obj = z_map_find(&reply.obj, "error");
		snprintf(err, errlen, "%s",
			(err_obj && err_obj->type == Z_STR && err_obj->val.str)
				? err_obj->val.str : "ssh: net refused the request");
		return false;
	}

	tok_obj = z_map_find(&reply.obj, "token");
	if (!tok_obj || tok_obj->type != Z_UINT32) {
		snprintf(err, errlen, "ssh: net returned no token");
		return false;
	}
	token = tok_obj->val.uint32;

	// Deliberately the same SCALAR shape telnet uses -- the token
	// stands in for telnet's target IP, and net tells them apart by
	// checking the value against the token it just issued.
	strcpy(t->provider, "net0");
	t->arg = z_obj_uint32(token);
	t->timeout_ticks = Z_CONN_TIMEOUT_NETWORK_TICKS;
	set_ip_detail(t, host, ip);

	return true;

}

// -- entry points ----------------------------------------------

bool z_conn_prepare(z_conn_kind_t kind, const char *text,
	z_conn_target_t *out, char *err, size_t errlen) {

	if (!out || !err || !errlen) return false;

	memset(out, 0, sizeof(*out));
	out->kind = kind;
	err[0] = 0;

	switch (kind) {
	case Z_CONN_PORT:   return prep_port(text, out, err, errlen);
	case Z_CONN_SERIAL: return prep_serial(text, out, err, errlen);
	case Z_CONN_TELNET: return prep_telnet(text, out, err, errlen);
	case Z_CONN_SSH:    return prep_ssh(text, out, err, errlen);
	default:
		snprintf(err, errlen, "unknown connection kind");
		return false;
	}

}

void z_conn_handoff(uint32_t term_pid, const z_conn_target_t *target) {

	z_obj_t map;

	if (!term_pid || !target) return;

	// Never freed -- see zconnect.h on why.
	map = z_obj_map(2);
	z_map_set(&map, "name", z_obj_str(target->provider));
	z_map_set(&map, "arg", target->arg);

	z_msg_new_send(term_pid, Z_TERM_SET_PORT, 0, map);

}
