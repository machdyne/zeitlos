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

// Asks net to print its link counters on the serial console -- the
// esp32link debug dump (crc errors, fifo overruns, input events
// dispatched), which used to be reachable only on an association
// failure. Carries no payload and gets no reply; the dump IS the
// answer. sh.c's `ic` sends this, so "where did the input go?" is a
// console question instead of an instrumented build.
#define Z_NET_DEBUG_DUMP         313

// sw/common/zweb.h has taken 310-312. The next subject added anywhere in
// this shared sequence starts at 318.

// -- accepting connections: Z_NET_LISTEN (docs/netserve.md) --
//
//   app -> net   Z_NET_LISTEN        tag: yours   obj Z_UINT32 port
//   net -> app   Z_NET_LISTEN_REPLY  same tag     obj Z_UINT32 0, or an error:
//                                                  Z_NET_LISTEN_E_*
//   app -> net   Z_NET_UNLISTEN      tag: any     obj Z_UINT32 port; no reply
//
// Each connection accepted on a listened port then arrives at the
// listening process as a zport CONNECT -- the process is the PROVIDER,
// net the client -- and from there it is an ordinary port connection
// (zport.h): DATA both ways, acked; CLOSE from either side.
//
//   net -> app   Z_PORT_CONNECT    tag: net's relay id   obj Z_BLOB z_net_accept_t
//   app -> net   Z_PORT_CONNECTED  tag: THE SAME relay id   obj Z_UINT32 your conn_id
//            or  Z_PORT_REFUSED    tag: the same relay id
//
// The tag is what z_port_accept()/z_port_refuse() do NOT do (they reply
// with tag 0): a process with several connections arriving at once has
// to say which one it is answering. Reply with z_msg_new_send().
//
// The blob is net's, and valid until the connection ends; copy what you
// need while handling the CONNECT.
//
// Listening is last-writer-wins: a second LISTEN for a port from another
// process takes it over (after a restart, the new netserve reclaims its
// ports) and the old owner's connections are closed. Apps are trusted
// (docs/security.md).
#define Z_NET_LISTEN             314
#define Z_NET_LISTEN_REPLY       315
#define Z_NET_UNLISTEN           316

// net -> listener, on an accepted connection: the peer has FINISHED
// SENDING (a TCP half-close, its FIN), and every byte it sent before
// that has been delivered as DATA. The connection is still open the
// other way: send what is left, then CLOSE as usual. Tag: your conn_id.
// No payload, no reply. zport has no half-close of its own; this is it.
#define Z_NET_EOF                317

#define Z_NET_LISTEN_E_PORT      1   // 0 or above 65535
#define Z_NET_LISTEN_E_FULL      2   // net's listen table is full
#define Z_NET_LISTEN_E_NOIP      3   // no address yet

#define Z_NET_ACCEPT_LOCAL       1u  // the peer is on this machine's own subnet

typedef struct {
	uint32_t ip;            // the peer
	uint16_t port;          // the peer's port
	uint16_t lport;         // ours: which listened port
	uint32_t flags;         // Z_NET_ACCEPT_*
} z_net_accept_t;

// -- Z_PORT_CONNECT to net: three meanings, told apart by SHAPE --
//
// `net` is a zport provider (sw/common/zport.h, docs/ports.md) for
// three different kinds of session, each a connection from tcp.c's pool
// and all arriving on the one Z_PORT_CONNECT subject. There is no
// discriminator field; they are distinguished by the type of the
// argument object:
//
//   Z_MAP { "ip": Z_UINT32, "port": Z_UINT32 }
//       A RAW TCP SOCKET. Everything sent through the port goes out
//       verbatim and everything received comes back verbatim -- no
//       telnet option stripping, no IAC escaping. Requires a build
//       with NET_SOCK=1 (the default); see sw/apps/net/sock.h.
//
//       This is what sw/apps/web uses, so that HTTP and TLS can live
//       in that app rather than in this one -- see docs/web_app.md.
//
//   Z_UINT32, matching a live token from Z_NET_SSH_PREPARE
//       An SSH session. See the long comment above.
//
//   Z_UINT32, anything else
//       A telnet session to that IP on port 23.
//
// The map is tested FIRST, because a type test cannot be wrong where
// the ssh token test has to guess.
//
// Note that a map is safe here, where the SSH handshake above had to
// go to great lengths to keep its argument scalar. The reason is the
// number of hops, not the type: the pointer double-translation
// described above happens when one process READS a message object and
// then RE-SENDS it, as `term` does. A socket client builds this map
// and sends it straight to `net`, so it is translated exactly once and
// nothing forwards it.
//
// ONE OF EACH KIND: one socket, one telnet and one ssh session at a
// time, but they no longer exclude each other -- tcp.c has a pool
// (docs/networking.md, "Connections").

#endif
