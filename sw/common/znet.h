#ifndef ZNET_H
#define ZNET_H

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Networking app protocol -- shared between the net app
 * (sw/apps/net) and any process that wants to use it (the shell's
 * tget/tput commands, sw/os/sh.c, or any other app later). See
 * docs/networking.md.
 *
 * Both directions of a TFTP transfer move through sw/common/zstream.h
 * as a stream now, not a single message carrying the whole file --
 * see tftp.h for why. That changes how GET and PUT are each kicked
 * off:
 *
 * GET: net.c is the data source, so use zstream_open() (see
 * zstream.h) directly against Z_PID_NET, with a
 * Z_MAP{"ip":Z_UINT32, "filename":Z_STR} payload. net.c streams the
 * received data back as the reply stream -- Z_STREAM_EOF marks
 * successful completion, Z_STREAM_ERROR carries a failure message.
 * There's currently only one thing an OPEN to net.c can mean, so no
 * separate subject/discriminator is needed; if net.c gains other
 * producer roles later, an "op" key in the payload can disambiguate
 * then.
 *
 * PUT: the direction is reversed (net.c needs to pull data FROM the
 * requester, not push it), so this can't reuse zstream_open() the
 * same way -- Z_NET_TFTP_PUT below just tells net.c to start a PUT
 * and gives it enough to open its OWN stream back to the requester,
 * which is the actual data channel. The requester needs to be ready
 * to act as a zstream producer (respond to net's Z_STREAM_OPEN)
 * essentially as soon as it sends this.
 *
 * DNS: a single request/reply pair, Z_NET_DNS_RESOLVE /
 * _RESOLVE_REPLY below -- see their own comments, and
 * sw/common/zdns.h for the blocking wrapper most callers should use
 * instead of sending these directly.
 */

// the networking app's well-known pid, same convention as Z_PID_WM
// (zwm.h): sh.c runs as pid 0, so starting wm then net in that order
// (`run wm`, `run net`) reserves pid 1 for wm and pid 2 for net. The
// shell's `init` command does this without needing wm at all, for
// testing net in isolation -- see docs/networking.md. There's no
// dynamic discovery yet (see docs/messaging.md), so this is a hard
// assumption until a real registry exists.
#define Z_PID_NET   2

// -- message subjects --

// requester -> net: Z_MAP{"ip":Z_UINT32, "filename":Z_STR}. net opens
// a stream back to the requester (see zstream.h) to pull the file's
// bytes, then forwards each chunk to the TFTP server as it arrives.
#define Z_NET_TFTP_PUT         302

// net -> requester, reply to Z_NET_TFTP_PUT (same tag): Z_MAP with
// "ok" (Z_UINT32, 0 or 1). If ok, nothing else. If not ok, "error"
// (Z_STR) holds a message. Sent once the whole transfer -- including
// the remote server's handling of the final block -- completes, not
// when the requester finishes producing chunks (those two can finish
// at different times).
#define Z_NET_TFTP_PUT_REPLY   303

// requester -> net: Z_STR (the hostname to resolve, e.g.
// "example.com"). net.c dispatches this to sw/apps/net/dns.c's
// dns_resolve_start(), which sends the actual DNS query (RFC 1035, A
// records only) to whichever nameserver is configured (DHCP-provided
// by default, or the NET_STATIC_DNS build-time override -- see
// docs/networking.md's "DNS client" section and dns.c's own header
// comment). One resolution in flight at a time, same "one X at a
// time" simplification as everything else in this app (TFTP's one
// transfer, TCP's one connection) -- a request that arrives while
// one is already pending gets an immediate "busy" error reply rather
// than being queued.
//
// Most callers don't need to send this directly: sw/common/zdns.h's
// z_dns_resolve()/z_resolve_host() wrap the send-and-wait-for-reply
// round trip below into a single blocking call, the same way
// zstream.h's blocking API wraps Z_STREAM_*'s own request/reply
// shape. Written directly here mainly for net.c/dns.c's own
// documentation purposes.
#define Z_NET_DNS_RESOLVE        304

// net -> requester, reply to Z_NET_DNS_RESOLVE (same tag): Z_MAP with
// "ok" (Z_UINT32, 0 or 1). If ok, "ip" (Z_UINT32) holds the resolved
// address. If not ok, "error" (Z_STR) holds a short reason (no
// nameserver configured, NXDOMAIN/no A record, timeout, busy with
// another resolution, etc).
#define Z_NET_DNS_RESOLVE_REPLY  305

// -- SSH session setup (sw/apps/net/ssh/, sw/apps/repl/repl.c) --
//
// NUMBERED 308/309, NOT 306/307. The subject space for `net` is split
// across TWO headers: this file holds 302-305, and sw/common/zntp.h
// continues the SAME sequence at 306-307. zntp.h says so in its own
// header and asks that the next subject added here start at 308.
//
// These were originally added at 306/307 by reading this file alone.
// The result was that every `ssh` command was delivered to net's NTP
// sync handler -- the dispatch chain matched Z_NET_NTP_SYNC first --
// so repl waited forever for a reply that was never going to come,
// and net logged nothing unusual because it had genuinely handled the
// message. Exactly the failure zntp.h predicted. net.c now carries a
// compile-time check (see its SUBJECT COLLISION CHECK) so the next
// one cannot reach a board.
//
// A two-step handshake that exists to solve one specific problem: an
// SSH session needs a USERNAME, and `term`'s Z_TERM_SET_PORT cannot
// carry one.
//
// The obvious design -- put {user, ip} in SET_PORT's `arg` map and let
// term forward it -- is broken, and subtly. z_resolve_obj()
// (sw/os/msg.c) rewrites payload pointers to PHYSICAL addresses when a
// message is read. When `term` then re-sends that same object in its
// own Z_PORT_CONNECT, the kernel translates it a SECOND time --
// `ptr - 0x80000000 + base` on an address that is already physical,
// which underflows into garbage. Telnet escapes this only because its
// arg is a bare Z_UINT32 with no pointers in it.
//
// So `repl` sends the strings directly to `net` in ONE hop, where they
// resolve correctly, and gets back an opaque token. SET_PORT then
// carries only that token, staying scalar exactly like telnet's IP.
//
// The token is also a nonce: it is consumed on use and expires, so a
// stale or misdirected CONNECT cannot pick up someone else's
// credentials.
//
// Request obj: Z_MAP { "user": Z_STR, "ip": Z_UINT32, "port": Z_UINT32 }
// Reply obj:   Z_MAP { "ok": Z_UINT32, "token": Z_UINT32,
//                      "error": Z_STR (only when ok == 0) }
#define Z_NET_SSH_PREPARE        308
#define Z_NET_SSH_PREPARE_REPLY  309


#endif
