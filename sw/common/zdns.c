/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * See zdns.h.
 */

#include <stdint.h>
#include <stdbool.h>

#include "zdns.h"
#include "zobj.h"
#include "zmsg.h"
#include "znet.h"

// deliberately NOT #include "zeitlos.h" -- see zdns.h's own header
// comment ("Why this builds into both an app and the kernel
// unmodified"), same reasoning zstream.c already documents for
// itself. Both the app runtime (zeitlos.c) and the kernel's own
// msg.c/pidreg.c provide matching signatures for all five of these.
z_rv z_msg_send(z_msg_t *msg);
z_rv z_msg_read(z_msg_t *msg);
z_rv z_msg_new_send(uint32_t to, uint32_t subject, uint32_t tag, z_obj_t obj);
uint32_t z_uptime_ticks(void);
bool z_pid_lookup(const char *name, uint32_t *pid);

// ~5s -- generous relative to dns.c's own retry budget
// (DNS_MAX_RETRIES * DNS_TIMEOUT_TICKS there, comfortably under this)
// so a real "no nameserver responding" failure is reported by dns.c
// itself, with a specific reason, well before this outer timeout
// would ever fire on its own. This one mainly covers "net isn't even
// running" (no reply will EVER come) and genuinely pathological
// message-queue delays, not the normal DNS-timeout path.
#define ZDNS_TIMEOUT_TICKS  (5 * 732)

static void set_err(char *err, uint32_t err_len, const char *msg) {
	if (!err || !err_len) return;
	uint32_t i = 0;
	for (; msg[i] && i < err_len - 1; i++) err[i] = msg[i];
	err[i] = 0;
}

bool z_parse_ipv4(const char *s, uint32_t *out) {

	uint32_t octets[4];

	for (int i = 0; i < 4; i++) {

		if (i > 0) {
			if (*s != '.') return false;
			s++;
		}

		if (*s < '0' || *s > '9') return false;

		uint32_t v = 0;
		int digits = 0;
		while (*s >= '0' && *s <= '9') {
			v = v * 10 + (*s - '0');
			s++;
			digits++;
			if (digits > 3 || v > 255) return false;
		}

		octets[i] = v;

	}

	if (*s != '\0') return false;	// trailing garbage

	*out = (octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) | octets[3];
	return true;

}

// net registers itself as "net0" (net.c's own z_pid_register() call)
// -- same lookup-with-fixed-pid-fallback pattern sh.c's own
// resolve_net_pid()/zwin.c's resolve_wm_pid() already use, just not
// cached across calls the way sh.c's is: this file has no single
// process lifetime to cache against (it's linked into many different
// processes, each with their own idea of "for how long is this
// valid"), and a fresh z_pid_lookup() is cheap enough that it's not
// worth the extra static state to avoid repeating it.
// Returns 0 if net isn't running. NO fallback to the fixed Z_PID_NET
// constant, deliberately.
//
// That fallback only ever applied when the lookup MISSED -- i.e. when
// net had not registered or was not running -- and in exactly those
// cases sending to a hardcoded pid delivers to whatever process
// happens to occupy it. Silent misdelivery is worse than an error,
// and the pid net lands on depends on start order (it is only 2 when
// wm was started first).
//
// net now registers before it touches any hardware, so a miss here
// means it genuinely is not there.
static uint32_t resolve_net_pid(void) {
	uint32_t pid;
	if (z_pid_lookup("net0", &pid)) return pid;
	return 0;
}

static uint32_t next_dns_tag(void) {
	static uint32_t tag = 0;
	return ++tag;
}

bool z_dns_resolve(const char *hostname, uint32_t *out_ip,
	char *err, uint32_t err_len) {

	if (!hostname || !hostname[0]) {
		set_err(err, err_len, "dns: empty hostname");
		return false;
	}

	uint32_t net_pid = resolve_net_pid();
	if (!net_pid) return false;	// net not running -- say so, don't guess
	uint32_t tag = next_dns_tag();

	z_msg_new_send(net_pid, Z_NET_DNS_RESOLVE, tag, z_obj_str(hostname));

	uint32_t start = z_uptime_ticks();

	while ((z_uptime_ticks() - start) < ZDNS_TIMEOUT_TICKS) {

		z_msg_t msg;
		if (z_msg_read(&msg) != Z_OK) continue;

		if (msg.subject != Z_NET_DNS_RESOLVE_REPLY || msg.tag != tag)
			continue;	// not a reply to THIS request -- discard and
						// keep waiting, same as zport.c's own
						// connect-timeout loop does for anything that
						// isn't the reply it's waiting for

		z_obj_t *ok_obj = z_map_find(&msg.obj, "ok");

		if (!ok_obj || ok_obj->type != Z_UINT32 || !ok_obj->val.uint32) {
			z_obj_t *err_obj = z_map_find(&msg.obj, "error");
			if (err_obj && err_obj->type == Z_STR && err_obj->val.str)
				set_err(err, err_len, err_obj->val.str);
			else
				set_err(err, err_len, "dns: resolve failed");
			return false;
		}

		z_obj_t *ip_obj = z_map_find(&msg.obj, "ip");
		if (!ip_obj || ip_obj->type != Z_UINT32) {
			set_err(err, err_len, "dns: malformed reply from net");
			return false;
		}

		*out_ip = ip_obj->val.uint32;
		return true;

	}

	set_err(err, err_len,
		"dns: no reply from net (is it running? `run net`)");
	return false;

}

bool z_resolve_host(const char *s, uint32_t *out_ip,
	char *err, uint32_t err_len) {

	if (z_parse_ipv4(s, out_ip)) return true;
	return z_dns_resolve(s, out_ip, err, err_len);

}
