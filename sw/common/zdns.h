#ifndef ZDNS_H
#define ZDNS_H

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Small shared helpers for turning something a person typed --
 * "192.168.1.1" or "myserver.local" -- into a packed uint32_t IP
 * address, from anywhere in Zeitlos: a normal app (repl's `telnet`
 * command, the primary caller this was built for) or the kernel
 * shell (sh.c's `tget`/`tput`, which gained the same hostname support
 * essentially for free by switching over to this file). See
 * docs/networking.md's "DNS client" section for the full design of
 * the actual resolver this calls into (sw/apps/net/dns.c) and the
 * wire protocol (znet.h's Z_NET_DNS_RESOLVE/_REPLY) this wraps.
 *
 * z_parse_ipv4() does no networking at all -- it's here because
 * z_resolve_host() needs it internally, and because sh.c and repl.c
 * both used to keep their own private copies of the exact same
 * function (see the git history around this file) purely because
 * there was nowhere shared both build contexts could reach. That's
 * exactly the problem this file's own dual-build trick (below) now
 * solves for real, so both of those copies were deleted in favor of
 * this one.
 *
 * -- Why this builds into both an app and the kernel unmodified --
 *
 * Same technique sw/common/zstream.c already established (see its
 * own header comment): z_msg_send()/z_msg_read()/z_msg_new_send()/
 * z_uptime_ticks()/z_pid_lookup() are forward-declared in zdns.c
 * instead of pulled in via #include "zeitlos.h", which would collide
 * with kruntime.c's own getch()/readline()/echo()/noecho() in the
 * kernel build (sh.c is pid 0, so it links msg.o/pidreg.o directly
 * rather than zeitlos.o -- see docs/networking.md's "sh.c: tget/tput
 * shell commands" section for the fuller story of why that split
 * exists at all). Both sides provide matching signatures for every
 * function zdns.c actually calls, so it links into either unmodified.
 */

#include <stdint.h>
#include <stdbool.h>

// parses a dotted-quad IPv4 address ("a.b.c.d") into a packed
// uint32_t, high octet first (the same layout every "ip" value in
// this codebase already uses -- see e.g. ip.c's own header
// construction). Returns false on anything malformed (out-of-range
// octet, wrong octet count, trailing garbage) rather than silently
// returning 0 -- 0.0.0.0 is itself a value someone could plausibly
// type by mistake, so treating a parse failure as "0" would be a
// silent, misleading success.
bool z_parse_ipv4(const char *s, uint32_t *out);

// resolves a hostname via net's DNS client (sw/apps/net/dns.c), over
// the Z_NET_DNS_RESOLVE/_REPLY messages znet.h defines. Blocking,
// with its own bounded timeout (ZDNS_TIMEOUT_TICKS, zdns.c) -- same
// "there's nothing else useful to do meanwhile, so just wait"
// reasoning z_port_connect_arg_timeout() (zport.c) and zstream.c's
// own open/pull calls already use elsewhere in this codebase. A
// caller on a message loop that ALSO needs to keep servicing other
// concurrent work while this blocks (repl.c's telnet command is a
// real example -- see its own comment at the call site) should know
// this stalls that loop for up to the full timeout in the worst
// case; short of building a second, real non-blocking resolver API
// for every caller, this is the same tradeoff already accepted
// throughout this codebase's other blocking helpers.
//
// Requires net to already be running (`run net`) and to have a
// nameserver configured (DHCP-provided by default, or the
// NET_STATIC_DNS build-time override -- see docs/networking.md).
// Returns false (leaving *out_ip untouched) if either of those isn't
// true, or if resolution genuinely fails (NXDOMAIN/no A record,
// timeout, net not running at all). err, if non-NULL, is filled with
// a short reason either way -- safe to pass NULL if the caller
// doesn't want to report it.
bool z_dns_resolve(const char *hostname, uint32_t *out_ip,
	char *err, uint32_t err_len);

// tries z_parse_ipv4() first (fast, no messaging, no dependency on
// net even being started); falls back to z_dns_resolve() only if
// that fails. This is the one function most callers actually want:
// "give me an address for whatever the user typed, dotted-quad or
// hostname, either is fine" -- see repl.c's `telnet` command for the
// intended use.
//
// A string that merely LOOKS like a malformed dotted-quad
// ("999.1.1.1", "1.2.3") still falls through to a DNS lookup rather
// than failing immediately here -- it's genuinely ambiguous whether
// that was a typo'd IP or just an unusual hostname, and the DNS
// lookup's own failure (whatever it turns out to be) is what actually
// surfaces to the caller via err in that case, same as any other
// hostname that doesn't resolve.
bool z_resolve_host(const char *s, uint32_t *out_ip,
	char *err, uint32_t err_len);

#endif
