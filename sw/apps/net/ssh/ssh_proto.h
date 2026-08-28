#ifndef SSH_PROTO_H
#define SSH_PROTO_H

#include <stdint.h>
#include <stdbool.h>

#include "ssh_sha256.h"
#include "ssh_crypto.h"

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * The SSH-2 client session engine: version exchange, key exchange,
 * user authentication, and one interactive shell channel.
 *
 * -- Why this has no idea TCP exists --
 *
 * Everything here is driven by two functions: ssh_proto_feed() pushes
 * received bytes in, and a caller-supplied write callback takes bytes
 * out. There is no tcp.h include, no zeitlos.h include, and no
 * reference to any Zeitlos type. ssh.c is the thin layer that binds
 * this to tcp.c and to net.c's zport provider.
 *
 * That is not architectural tidiness for its own sake. An SSH
 * handshake is the single hardest thing in this codebase to debug on
 * real hardware: it either completes or it produces one useless line
 * about a bad MAC, and every attempt costs a flash cycle. Keeping the
 * engine free of platform types means the whole handshake runs on a
 * host against a test server (sw/test/test_ssh_session.c) where a
 * wrong byte can be printed instead of guessed at. Every bug found
 * there is one not found with an oscilloscope.
 *
 * Preserve this property. If something in here ever needs the current
 * time or a random number, it takes a callback, it does not include a
 * header.
 *
 * -- What is implemented, and what is deliberately not --
 *
 * Exactly one algorithm per slot, because that is what sw/ext/
 * monocypher can do (see ssh_crypto.h):
 *
 *     kex          curve25519-sha256
 *     host key     ssh-ed25519
 *     cipher       chacha20-poly1305@openssh.com  (both directions)
 *     mac          implicit in the AEAD
 *     compression  none
 *
 * There is therefore NO ALGORITHM NEGOTIATION worth the name. We send
 * our one name per slot and check the server's list contains it. A
 * server that has disabled ChaCha20 gets a specific error rather than
 * a generic failure, because "this server does not offer the one
 * cipher we have" is something a user can act on.
 *
 * Not implemented, on purpose:
 *
 *   - Port forwarding, X11, agent forwarding, subsystems, SFTP. One
 *     interactive shell channel is the whole use case; each of those
 *     is a separate protocol on top.
 *   - Compression. `zlib` would need an inflate/deflate implementation
 *     bigger than this entire client.
 *   - Multiple channels. tcp.c has one TCB, so there is one connection
 *     and one session anyway.
 *
 * Implemented although it is tempting not to be:
 *
 *   - REKEY. OpenSSH forces one after an hour or a gigabyte. A client
 *     that ignores a server-initiated KEXINIT mid-session works
 *     perfectly in every test anyone runs and then dies at the one
 *     hour mark, which is a miserable bug to be handed later.
 *   - SSH_MSG_IGNORE / DEBUG / UNIMPLEMENTED / GLOBAL_REQUEST. Real
 *     servers send these unprompted; OpenSSH sends a
 *     `hostkeys-00@openssh.com` global request right after auth.
 *     Choking on them is not a theoretical failure.
 *   - Oversize packets. Streamed and discarded rather than fatal --
 *     see ssh_proto_feed().
 */

// -- events the engine raises to its owner --

typedef enum {
	// Progress worth showing a user during a handshake that can take
	// a couple of seconds. `text` is a short human-readable line.
	SSH_EV_STATUS,

	// The host key's fingerprint, in `text` ("SHA256:..."), raised
	// once per connection after the signature verifies. The owner
	// decides whether that key is acceptable (known_hosts, or asking)
	// and calls ssh_proto_accept_host() or ssh_proto_disconnect().
	// THE ENGINE STOPS AND WAITS -- it does not proceed on its own,
	// because proceeding is the decision that matters.
	SSH_EV_HOSTKEY,

	// A password is needed. The owner prompts and calls
	// ssh_proto_password(). Raised again on a retry.
	SSH_EV_NEED_PASSWORD,

	// The shell is open; `data` traffic flows from here.
	SSH_EV_READY,

	// Channel data from the server -- borrowed buffer, valid only for
	// the duration of the callback (same convention as tcp.h's
	// TCP_EVENT_DATA).
	SSH_EV_DATA,

	// Server sent us a banner, or stderr from the channel. Same
	// borrowed lifetime. Kept distinct from SSH_EV_DATA so an owner
	// can render it differently; net.c currently relays both.
	SSH_EV_BANNER,

	// The session is over. `text` says why -- either a clean exit or
	// the specific thing that went wrong. Always the last event.
	SSH_EV_CLOSED,
} ssh_event_t;

typedef struct ssh_proto ssh_proto_t;

// Bytes to be sent to the server. Return false if they could not be
// queued; the engine treats that as fatal, since there is no way to
// resynchronise an SSH stream with a hole in it.
typedef bool (*ssh_write_fn)(void *user, const uint8_t *data, uint32_t len);

typedef void (*ssh_event_fn)(void *user, ssh_event_t ev,
	const uint8_t *data, uint32_t len, const char *text);

// Fill `out` with `len` cryptographically secure random bytes. A
// callback rather than a direct z_rng_bytes() call so this file stays
// platform-free -- and so the test harness can inject known values to
// make a handshake reproducible.
//
// THE OWNER MUST HAVE CHECKED z_rng_secure() BEFORE GETTING HERE. This
// engine cannot verify the quality of what it is handed, and an
// ephemeral key from a weak source hands the entire session to a
// passive observer with no symptom at either end.
typedef void (*ssh_random_fn)(void *user, uint8_t *out, uint32_t len);

#ifndef SSH_MAX_PACKET
#define SSH_MAX_PACKET 4096
#endif

// Longest username/password we accept. Not a protocol limit; just the
// size of the fixed buffers, since this runs in `net` whose whole
// stack+heap allowance is 32KB (kernel.h).
#define SSH_MAX_USER 64
#define SSH_MAX_PASS 128

struct ssh_proto {

	// -- owner hooks --
	void *user;
	ssh_write_fn write;
	ssh_event_fn event;
	ssh_random_fn random;

	// -- connection parameters --
	char username[SSH_MAX_USER];
	char password[SSH_MAX_PASS];
	bool have_password;

	// -- state machine --
	int state;
	bool failed;

	// -- version exchange --
	char v_s[256];			// server's identification, CR/LF stripped
	uint32_t v_s_len;

	// -- receive framing --
	uint8_t rx[SSH_MAX_PACKET + SSH_AEAD_TAG_LEN + 8];
	uint32_t rx_len;		// bytes currently in rx
	uint32_t rx_need;		// total bytes wanted for the current packet
	uint32_t rx_payload;	// payload length once the header is decoded
	bool rx_header_done;
	bool rx_discard;		// oversize packet: consume and drop

	// -- key exchange --
	ssh_sha256_ctx hash;	// exchange hash, fed incrementally
	uint8_t eph_secret[32];
	uint8_t eph_public[32];
	uint8_t session_id[SSH_SHA256_DIGEST];
	bool have_session_id;
	uint8_t exchange_hash[SSH_SHA256_DIGEST];
	// Shared secret, mpint-encoded. 40 not 36: the worst case is 4
	// length bytes + 1 leading zero + 32 value bytes = 37, and 36 was
	// the first number written here. It fits whenever the secret's
	// top bit is CLEAR, so it works about half the time -- the exact
	// intermittent failure ssh_wire.h warns about, arrived at from the
	// other direction. Caught by sw/test/test_ssh_session.c on a
	// pinned PRNG that happened to produce a high-bit-set secret.
	uint8_t k_mpint[40];
	uint32_t k_mpint_len;
	bool rekeying;

	// -- ciphers --
	ssh_cipher tx_cipher;
	ssh_cipher rx_cipher;

	// -- channel --
	uint32_t local_chan;
	uint32_t remote_chan;
	uint32_t local_window;
	uint32_t remote_window;
	uint32_t remote_max_packet;

	// -- input held while stalled --
	//
	// ssh_proto_feed() cannot simply return without consuming: its
	// caller is a TCP receive callback with a borrowed buffer
	// (tcp.h's TCP_EVENT_DATA), so anything not taken here is gone
	// for good. While the engine is waiting on a host-key decision or
	// a password, arriving bytes land here and are drained the moment
	// the owner answers.
	//
	// Small on purpose. In practice a server sends NOTHING at either
	// stall point -- it is waiting for our NEWKEYS or our auth
	// request -- so this exists to make a rare case correct rather
	// than to buffer a stream. Overflow is a clean, specific failure,
	// not corruption.
	//
	// Found by sw/test/test_ssh_session.c, which lost data here on
	// its first run.
	uint8_t pend[1024];
	uint32_t pend_len;

	// -- misc --
	uint32_t auth_attempts;
	uint8_t tx[SSH_MAX_PACKET + SSH_AEAD_TAG_LEN + 8];
};

// Sets up `s` and emits the client identification string. `username`
// may be empty, in which case the owner is expected to supply one
// before authentication via ssh_proto_set_user().
void ssh_proto_init(ssh_proto_t *s, void *user, ssh_write_fn write,
	ssh_event_fn event, ssh_random_fn random, const char *username);

// Push received bytes in. Safe to call with any chunking -- the engine
// reassembles across calls, since an SSH packet routinely straddles
// TCP segments.
void ssh_proto_feed(ssh_proto_t *s, const uint8_t *data, uint32_t len);

void ssh_proto_set_user(ssh_proto_t *s, const char *username);
void ssh_proto_password(ssh_proto_t *s, const char *password);

// Continue after SSH_EV_HOSTKEY. Not calling this leaves the session
// stalled, which is the correct behaviour for an unverified key.
void ssh_proto_accept_host(ssh_proto_t *s);

// Send channel data (keystrokes) to the server. Returns false if the
// session is not open or the server's window is exhausted.
bool ssh_proto_send(ssh_proto_t *s, const uint8_t *data, uint32_t len);

// Begin a clean shutdown.
void ssh_proto_disconnect(ssh_proto_t *s, const char *why);

bool ssh_proto_is_open(const ssh_proto_t *s);

#endif
