#ifndef TLS_H
#define TLS_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * A TLS 1.3 client: the record layer and the handshake. See
 * docs/tls.md, and tls_crypto.h for everything cryptographic.
 *
 * -- transport agnostic, on purpose --
 *
 * This file never touches a socket. Ciphertext arrives through
 * tls_feed() and leaves through a callback, so the same code runs
 * against `net`'s raw socket on hardware and against a plain POSIX
 * socket on the build machine.
 *
 * That is what makes tests/test_tls.c possible: it runs this client
 * against a real OpenSSL server on localhost and completes a real
 * handshake. Debugging a handshake on an FPGA over a UART, against a
 * remote server that answers every mistake with the same
 * deliberately uninformative alert, is not a thing anyone should do
 * if they can avoid it.
 *
 * -- one suite, one group --
 *
 *   TLS_CHACHA20_POLY1305_SHA256   (0x1303)
 *   x25519                         (0x001d)
 *
 * Both because they are what this tree already has (tls_crypto.h) and
 * because between them they are accepted by essentially every TLS 1.3
 * server in existence. A server that offers neither gets a clear
 * error rather than a fallback.
 *
 * -- the server is authenticated --
 *
 * The chain is verified (verify.h) and the CertificateVerify
 * signature is checked before the handshake is allowed to complete.
 * A client that does the first without the second is not
 * authenticating anything: a chain is public, so anyone can replay
 * one. CertificateVerify is the proof that the peer holds the leaf's
 * private key.
 *
 * tls_set_verify() MUST be called before tls_start(). Without it, or
 * with a NULL root lookup, tls_start() refuses -- there is
 * deliberately no way to run this code with verification switched
 * off. The WEB_TLS_INSECURE flag that existed while the verification
 * machinery was being written is gone.
 *
 * That is the standard docs/ssh.md set: refuse, do not warn. A
 * warning shown on every connection is one that gets clicked
 * through, and nobody can tell an intercepted page from a working
 * one by looking at it.
 *
 * -- what is not implemented --
 *
 *   - No session resumption or 0-RTT. Both need a ticket store, and
 *     0-RTT additionally needs replay protection that a client cannot
 *     provide by itself.
 *   - No client certificates. A CertificateRequest is answered with
 *     an empty Certificate, which is what the RFC asks of a client
 *     that has none.
 *   - No HelloRetryRequest. Detected and reported as an error rather
 *     than misparsed. It only happens when a server rejects every
 *     group offered, and x25519 is universal.
 *   - No renegotiation. It does not exist in TLS 1.3.
 *   - KeyUpdate is HANDLED, because a long-lived connection will get
 *     one and ignoring it desynchronises the record layer.
 */

#include <stdint.h>
#include <stdbool.h>

// Pulled in up here rather than beside the context definition below,
// because tls_set_verify()'s prototype needs verify_root_fn.
#include "tls_crypto.h"
#include "verify.h"
#include "../../common/zsha256.h"

// The largest record on the wire: 2^14 of plaintext, plus the content
// type byte, plus the AEAD tag, plus the slack RFC 8446 section 5.2
// allows for padding.
#define TLS_MAX_RECORD  (16384 + 256)

// Longest handshake message reassembled in full.
//
// Anything larger is HASHED INTO THE TRANSCRIPT AND SKIPPED rather
// than buffered -- which is what makes a certificate chain of any
// size work in a phase that does not look at certificates. Every
// message this client actually parses (ServerHello, Finished) is a
// few hundred bytes at most.
#define TLS_MAX_HS_MSG  2048

// The Certificate message gets its own buffer, because it is the one
// handshake message that is routinely far larger than the rest put
// together -- a three-certificate chain with a 4096-bit root runs to
// several kilobytes. It cannot be streamed past like it used to be:
// verifying it means holding all of it.
//
// A chain that does not fit is a connection that fails, which is the
// right outcome. 12KB covers every public chain by a wide margin.
#define TLS_MAX_CERT    12288

typedef void (*tls_send_fn)(void *user, const uint8_t *data, uint32_t len);
typedef void (*tls_data_fn)(void *user, const uint8_t *data, uint32_t len);

typedef struct tls_ctx tls_ctx_t;

// `host` is used for SNI and is copied. `send` is called with
// ciphertext to put on the wire; `data` with decrypted application
// data. Both may be called re-entrantly from tls_feed().
void tls_init(tls_ctx_t *c, const char *host,
	tls_send_fn send, tls_data_fn data, void *user);

// Builds and emits the ClientHello.
//
// Returns false, without sending anything, if z_rng_secure() is
// false. A key derived from cycle-counter jitter is not a weak
// session, it is an open one, and the user cannot tell by looking --
// the same refusal sw/apps/net's SSH client makes, for the same
// reason (zrng.h).
// Supplies what the chain check needs. MUST be called before
// tls_start(), which refuses without it.
//
// `now` is Unix seconds; 0 means the clock is not set, and
// verify.h explains why that is a refusal rather than a licence to
// skip the date check.
void tls_set_verify(tls_ctx_t *c, int64_t now,
	verify_root_fn find_root, void *user);

bool tls_start(tls_ctx_t *c);

// Ciphertext from the socket.
void tls_feed(tls_ctx_t *c, const uint8_t *data, uint32_t len);

// Application data out. Fails before the handshake completes.
bool tls_write(tls_ctx_t *c, const uint8_t *data, uint32_t len);

// Sends close_notify. The socket may be closed immediately after;
// waiting for the peer's reply buys nothing here.
void tls_close(tls_ctx_t *c);

bool tls_established(const tls_ctx_t *c);

// The peer sent close_notify, or tls_close() was called. A clean end
// of stream, not an error -- for HTTP with `Connection: close` this
// is the normal way a response finishes.
bool tls_closed(const tls_ctx_t *c);
bool tls_failed(const tls_ctx_t *c);

// Handshake progress, for diagnosing a flight that never completes:
// records processed, handshake messages assembled, the type of the
// last one, and the internal state.
void tls_progress(const tls_ctx_t *c, uint32_t *records, uint32_t *msgs,
	uint8_t *last_type, uint8_t *state);

// NULL unless something went wrong. Short, and safe to show a person.
const char *tls_error(const tls_ctx_t *c);

// -- the context --------------------------------------------------
//
// Public only so it can be a static in the caller rather than an
// allocation. It is large -- a full record buffer dominates -- and
// there is exactly one per process.

struct tls_ctx {

	uint8_t			state;
	const char		*err;

	tls_send_fn		send;
	tls_data_fn		data;
	void			*user;

	char			host[256];

	// -- key exchange --
	uint8_t			priv[32];
	uint8_t			pub[32];
	uint8_t			client_random[32];
	uint8_t			session_id[32];

	// -- transcript --
	//
	// `transcript` accumulates every handshake message. `th_pre_msg`
	// is a copy of it taken immediately BEFORE the current message's
	// header is added.
	//
	// The second one exists for exactly one reason: a Finished
	// message's verify_data covers the transcript up to but NOT
	// including itself, and by the time the message has been
	// reassembled it is already in the running hash. Snapshotting
	// after the fact is impossible, so it is snapshotted before.
	z_sha256_ctx	transcript;
	z_sha256_ctx	th_pre_msg;

	// -- schedule --
	uint8_t			handshake_secret[TLS_HASH_LEN];
	uint8_t			master_secret[TLS_HASH_LEN];
	uint8_t			client_hs_secret[TLS_HASH_LEN];
	uint8_t			server_hs_secret[TLS_HASH_LEN];
	uint8_t			client_ap_secret[TLS_HASH_LEN];
	uint8_t			server_ap_secret[TLS_HASH_LEN];

	// -- record protection --
	uint8_t			rx_key[TLS_KEY_LEN], rx_iv[TLS_IV_LEN];
	uint8_t			tx_key[TLS_KEY_LEN], tx_iv[TLS_IV_LEN];
	uint64_t		rx_seq, tx_seq;
	bool			rx_encrypted, tx_encrypted;

	// -- record reassembly --
	// TLS_MAX_RECORD is the largest BODY; the 5-byte header is held
	// in front of it, so the buffer needs both.
	//
	// It was sized at TLS_MAX_RECORD alone, which overruns by exactly
	// five bytes on a maximum-length record -- reachable from the
	// network, since the length comes off the wire and is only
	// checked against TLS_MAX_RECORD.
	uint8_t			rec[TLS_MAX_RECORD + 5];
	uint32_t		rec_len;			// bytes held
	uint32_t		rec_want;			// full record size once known

	uint32_t		n_records;			// handshake records processed
	uint32_t		n_hs_msgs;			// handshake messages assembled
	uint8_t			last_hs_type;

	// -- certificate chain --
	int64_t			now;
	verify_root_fn	find_root;
	void			*root_user;
	bool			verify_ready;

	uint8_t			cert[TLS_MAX_CERT];
	uint32_t		cert_len;
	cert_blob_t		chain[VERIFY_MAX_CHAIN];
	uint32_t		chain_n;
	x509_cert_t		leaf;
	bool			have_leaf;
	bool			chain_ok;

	// The transcript hash through Certificate, which is what the
	// CertificateVerify signature covers.
	uint8_t			th_certs[TLS_HASH_LEN];
	bool			have_th_certs;

	// -- handshake message reassembly --
	uint8_t			hs[TLS_MAX_HS_MSG];
	uint8_t			*hs_dst;			// hs[] or cert[], chosen per message
	uint32_t		hs_cap;
	uint32_t		hs_len;				// bytes buffered
	uint32_t		hs_need;			// body bytes still expected
	uint8_t			hs_type;
	bool			hs_in_body;
	bool			hs_skipping;		// too big to buffer; hash only
	uint8_t			hs_hdr[4];
	uint32_t		hs_hdr_len;

};

#endif
