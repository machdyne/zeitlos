#ifndef SSH_SERVER_H
#define SSH_SERVER_H
/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The SSH-2 SERVER session engine: the other half of ssh_proto.c.
 * Version exchange, key exchange, password authentication, and one
 * interactive shell channel. netserve (sw/apps/net/netserve) binds it
 * to a TCP connection and a port. docs/netserve.md, "SSH".
 *
 * -- No platform in here --
 *
 * The same rule as ssh_proto.c, for the same reason: bytes in through
 * sshs_feed(), bytes out through a write callback, randomness and the
 * password check through callbacks. So the whole thing runs on a host
 * against the REAL client engine (sw/apps/net/ssh/tests/
 * test_ssh_server.c), and every byte of a handshake can be printed
 * rather than guessed at.
 *
 * -- What it speaks --
 *
 * The client's one suite, from the other side:
 *
 *     kex        curve25519-sha256 (and its @libssh.org alias)
 *     host key   ssh-ed25519
 *     cipher     chacha20-poly1305@openssh.com, both ways
 *     mac        implicit in the AEAD -- but a list is still offered
 *     compress   none
 *
 * STRICT KEX (kex-strict-s-v00@openssh.com) whenever the client offers
 * it, as OpenSSH 9.6 and later do. It is the fix for the Terrapin
 * attack (CVE-2023-48795), which works against exactly this cipher:
 * an attacker on the path deletes a message during the handshake and
 * adjusts the sequence numbers so nobody notices. Strict mode resets
 * the numbers at every NEWKEYS and refuses anything but key exchange
 * messages during the first one.
 *
 * Password authentication only. "none", "publickey" and anything else
 * get a FAILURE listing "password", which every client handles. One
 * `session` channel; `pty-req` and `shell` accepted, `exec`,
 * `subsystem` (sftp, scp) and forwarding refused.
 *
 * -- Flow control, both ways --
 *
 * Client to server: the window we advertise is SSHS_LOCAL_WINDOW, the
 * size of the buffer the owner holds for it, and it is only topped up
 * (sshs_consumed()) as the owner passes bytes on. A client that sends
 * past it is disconnected.
 *
 * Server to client: sshs_send() takes no more than the client's window
 * and maximum packet allow, and none at all during a rekey, when only
 * key exchange messages may be sent.
 */
#include <stdint.h>
#include <stdbool.h>

#include "ssh_sha256.h"
#include "ssh_crypto.h"

#ifndef SSHS_MAX_PACKET
#define SSHS_MAX_PACKET   4096      // a client's KEXINIT is ~1.5 KB
#endif
#define SSHS_LOCAL_WINDOW 1024      // the owner must hold this much, for sshs_feed()'s DATA
#define SSHS_TX_MAX       1200      // the largest packet we build
#define SSHS_SEND_OVERHEAD 48       // what a CHANNEL_DATA packet adds to its data

typedef enum {
	SSHS_EV_LOG,        // text: a line for the console
	SSHS_EV_SHELL,      // the client wants a shell: connect the session to its port
	SSHS_EV_DATA,       // channel data from the client; borrowed, as tcp.h's DATA
	SSHS_EV_EOF,        // the client will send no more channel data
	SSHS_EV_CLOSED,     // over; text says why. The last event.
} sshs_event_t;

// Bytes to send to the client. False is fatal: an SSH stream with a
// hole in it cannot be resynchronised.
typedef bool (*sshs_write_fn)(void *user, const uint8_t *data, uint32_t len);
typedef void (*sshs_event_fn)(void *user, sshs_event_t ev, const uint8_t *data,
	uint32_t len, const char *text);
// Cryptographically secure random bytes. The owner has checked its
// source is seeded (z_rng_secure()) before starting a session.
typedef void (*sshs_random_fn)(void *user, uint8_t *out, uint32_t len);

#define SSHS_AUTH_OK      0
#define SSHS_AUTH_BAD     1
#define SSHS_AUTH_REFUSE  2         // e.g. the kernel's backoff: disconnect
typedef int (*sshs_auth_fn)(void *user, const char *username,
	const uint8_t *password, uint32_t len);

typedef struct {
	// hooks
	void *user;
	sshs_write_fn write;
	sshs_event_fn event;
	sshs_random_fn random;
	sshs_auth_fn auth;

	// the host key
	uint8_t host_secret[64];        // Ed25519 secret key (seed || public)
	uint8_t host_blob[51];          // string("ssh-ed25519") || string(public)

	int state;
	bool strict;                    // strict KEX agreed
	bool first_kex_done;
	bool kexing;                    // between KEXINITs and NEWKEYS: no channel data
	bool our_kexinit_sent;          // in this exchange
	bool ignore_next;               // the client guessed its kex packet wrong

	// version exchange
	char v_c[256];
	uint32_t v_c_len;

	// receive framing (as ssh_proto.c)
	uint8_t rx[SSHS_MAX_PACKET + SSH_AEAD_TAG_LEN + 8];
	uint32_t rx_len, rx_need, rx_payload;
	bool rx_header_done;

	uint8_t tx[SSHS_TX_MAX + SSH_AEAD_TAG_LEN + 8];

	// key exchange
	ssh_sha256_ctx hash;
	uint8_t i_s[512];               // our KEXINIT payload, for the exchange hash
	uint32_t i_s_len;
	uint8_t eph_secret[32];
	uint8_t session_id[SSH_SHA256_DIGEST];
	bool have_session_id;
	uint8_t exchange_hash[SSH_SHA256_DIGEST];
	uint8_t k_mpint[40];
	uint32_t k_mpint_len;
	ssh_cipher tx_cipher, rx_cipher;

	// authentication
	char username[33];
	uint8_t auth_failures;
	bool authed;

	// the channel
	bool chan_open, chan_eof_sent, chan_close_sent, shell;
	uint32_t remote_chan;
	uint32_t remote_window, remote_max;
	uint32_t local_window;          // what the client may still send
	uint32_t local_credit;          // passed on by the owner, not yet re-advertised
} ssh_server_t;

// Starts a session: sends our identification and KEXINIT. host_seed is
// the 32-byte Ed25519 seed; the engine derives the key pair from it.
void sshs_init(ssh_server_t *s, const uint8_t host_seed[32], void *user,
	sshs_write_fn write, sshs_event_fn event, sshs_random_fn random, sshs_auth_fn auth);

// Bytes from the client. Always consumes all of them.
void sshs_feed(ssh_server_t *s, const uint8_t *data, uint32_t len);

// Channel data to the client: sends at most what the client's window
// and maximum packet allow, and what fits `room` bytes of output
// (SSHS_SEND_OVERHEAD more than the data). Returns how much it took;
// 0 when it can take nothing now -- try again later.
uint32_t sshs_send(ssh_server_t *s, const uint8_t *data, uint32_t len, uint32_t room);

// The owner has passed `n` bytes of the client's data on: they may be
// advertised again (WINDOW_ADJUST).
void sshs_consumed(ssh_server_t *s, uint32_t n);

// The session's port closed: exit-status, EOF and CLOSE to the client,
// then SSHS_EV_CLOSED -- close the connection once they have gone out.
// The client's own CLOSE is not waited for (ours sends none).
void sshs_end(ssh_server_t *s, uint32_t exit_status);

// Ends it now, with a DISCONNECT the client will show.
void sshs_disconnect(ssh_server_t *s, const char *why);

bool sshs_is_closed(const ssh_server_t *s);

// The host key's public half and its fingerprint ("SHA256:..."), for
// the console at startup: a user checks it on their first connection.
void sshs_host_public(const uint8_t host_seed[32], uint8_t pub[32], char *fp, uint32_t cap);

#endif
