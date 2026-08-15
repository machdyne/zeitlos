#ifndef Z_STREAM_H
#define Z_STREAM_H

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Generic streaming, built entirely on top of zmsg.h -- a pull-based
 * pipe for moving data too large (or too incremental) to hand over
 * as a single message, between two processes, in bounded memory on
 * both ends.
 *
 * Why pull-based: a mailbox is small on purpose (Z_MAILBOX_DEPTH, see
 * zmsg.h) -- it's meant to be drained promptly, not act as a queue.
 * A producer that just pushes chunks as fast as it can would overrun
 * that. Pull-based sidesteps the whole problem by construction: the
 * consumer asks for exactly one chunk at a time, so there's never
 * more than one chunk in flight, and the request/reply rhythm *is*
 * the flow control -- no separate credit/window scheme needed.
 *
 * Just as important: the producer side never blocks. It's fed
 * messages the caller's own loop already read (see
 * zstream_producer_handle() below) rather than doing its own
 * z_msg_read()/wait -- a producer like `net` has to keep servicing
 * its own underlying work (e.g. the actual network I/O) the whole
 * time it's also streaming to a consumer, so a producer-side call
 * that blocks waiting on the consumer would stall that other work
 * for as long as the consumer takes to respond. The consumer side
 * *is* allowed to block (z_stream_open()/z_stream_pull() below) --
 * a consumer with nothing else to do while it waits (e.g. the shell)
 * doesn't need to poll.
 *
 * Why this also keeps both sides allocation-safe: z_obj_blob() (like
 * z_obj_map()) mallocs on every call, and the normal messaging
 * pattern elsewhere in this codebase (see net.c's TFTP reply) never
 * frees the top-level object it hands to z_msg_send() -- fine for a
 * single one-shot reply, not fine repeated per-chunk across a stream
 * that might be thousands of chunks long. The pull protocol itself
 * gives us the fix for free: the producer keeps exactly one
 * outstanding chunk (the last one it sent) alive, and frees it the
 * moment the *next* pull arrives -- receipt of that next pull is
 * itself proof the consumer already has the previous chunk, satisfying
 * zmsg.h's "valid until your own next send" rule with no separate ack.
 * Net effect: exactly one chunk's worth of heap allocation outstanding
 * at any moment, on either side, for a stream of any length.
 *
 * Wire protocol, all built from ordinary z_msg_t messages:
 *
 *   OPEN        consumer -> producer  tag=nonce   obj=caller-supplied
 *                 (opaque to zstream -- e.g. TFTP's existing GET/PUT
 *                 request shape. zstream doesn't know or care what's
 *                 inside; the producer application interprets it.)
 *   OPEN_REPLY  producer -> consumer  tag=nonce   obj=Z_UINT32(stream_id) on
 *                 success, or Z_STR(error) on failure (stream_id left at 0)
 *
 *   from here on, every message's tag is (stream_id<<16 | seq&0xffff)
 *   -- packs both into the existing tag field for free, ordinary
 *   subject+tag matching demultiplexes everything with no extra
 *   allocation and no wrapper object needed.
 *
 *   PULL        consumer -> producer  obj=Z_NONE   "send me chunk seq"
 *   CHUNK       producer -> consumer  obj=Z_BLOB    the chunk's data
 *   EOF         producer -> consumer  obj=Z_NONE    no more chunks, done
 *   ERROR       producer -> consumer  obj=Z_STR     stream failed, done
 *   ABORT       consumer -> producer  obj=Z_NONE    consumer giving up early
 *
 * A pull for the same seq as the last chunk already sent is treated
 * as a retry (the producer still has that chunk's blob on hand) and
 * gets the same data resent, rather than being treated as an error --
 * cheap robustness against a lost reply, though on purely local,
 * in-memory messaging this is expected to be rare to never in
 * practice.
 *
 * This header defines the wire protocol and constants shared by both
 * sides, plus the producer API (below) which is fed messages by the
 * caller's own loop and so has no dependency on which z_msg_send()/
 * z_msg_read() implementation is linked. zstream.c also provides a
 * blocking consumer API built directly on z_msg_send()/z_msg_read()/
 * z_uptime_ticks(), which resolve at link time to whichever
 * implementation is actually linked (zeitlos.o for apps, msg.o for
 * the kernel -- see msg.h's comment on z_msg_send for why that split
 * exists and how it stays transparent to shared code like this).
 */

#include <stdint.h>
#include <stdbool.h>
#include "zobj.h"
#include "zmsg.h"

#define Z_STREAM_OPEN         400
#define Z_STREAM_OPEN_REPLY   401
#define Z_STREAM_PULL         402
#define Z_STREAM_CHUNK        403
#define Z_STREAM_EOF          404
#define Z_STREAM_ERROR        405
#define Z_STREAM_ABORT        406

// suggested default -- not part of the wire protocol itself (a
// chunk's size is just its Z_BLOB's own len field, self-describing),
// just a sensible starting point for producers that don't otherwise
// have a natural unit. TFTP's own block size is a natural fit for
// TFTP-backed streams specifically.
#define ZSTREAM_CHUNK_SIZE_DEFAULT   512

// pack/unpack the (stream_id, seq) pair into a single tag, used for
// every message from PULL onward. stream_id and seq are each capped
// at 16 bits by this -- 65536 concurrent streams per producer and
// 65536 chunks per stream (32MB+ at the default chunk size) is
// generous headroom for what this is for; seq wrapping past that
// isn't specially handled, matching the "not a general-purpose queue"
// spirit of the rest of the messaging system.
#define ZSTREAM_TAG(stream_id, seq) \
	((((uint32_t)(stream_id) & 0xffff) << 16) | ((uint32_t)(seq) & 0xffff))
#define ZSTREAM_TAG_STREAM_ID(tag)  (((uint32_t)(tag) >> 16) & 0xffff)
#define ZSTREAM_TAG_SEQ(tag)        ((uint32_t)(tag) & 0xffff)

typedef enum {
	ZSTREAM_EVENT_NONE = 0,	// msg wasn't a stream-protocol msg for this stream
	ZSTREAM_EVENT_PULL,	// consumer wants the next chunk -- caller should now
				// call zstream_send_chunk/eof/error
	ZSTREAM_EVENT_ABORT	// consumer gave up -- caller should clean up and
				// stop producing
} zstream_event_t;

typedef enum {
	ZSTREAM_CHUNK = 0,	// got a chunk; *data/*len point into borrowed storage
	ZSTREAM_EOF,		// stream complete, no more chunks
	ZSTREAM_ERROR		// stream failed; err (if given) has the message
} zstream_result_t;

// -- producer side --
//
// never blocks, never does its own z_msg_read() -- fed messages the
// caller's own loop already read (matching net.c's existing
// drain-then-dispatch pattern). safe to use from either an app or
// the kernel with no changes, since it never touches z_msg_send/read
// directly either (all the actual sending funnels through the small
// set of static helpers in zstream.c, which do use them, and resolve
// the same way the consumer side's calls do).

typedef struct {

	uint32_t	stream_id;
	uint32_t	consumer_pid;
	uint32_t	seq;		// seq of the chunk we last sent (or
					// UINT32_MAX before the first chunk)
	bool		active;

	z_obj_t		last_chunk;	// kept alive until the next pull
					// proves the consumer has it

} zstream_producer_t;

// accept/reject a just-received Z_STREAM_OPEN message. the caller's
// own message loop is what actually receives that message -- pass
// along msg->from and msg->tag from it. zstream_accept() picks a
// fresh stream_id and initializes *st; zstream_reject() doesn't need
// a live producer struct at all, for the case where the request
// itself is invalid.
void zstream_accept(zstream_producer_t *st, uint32_t consumer_pid, uint32_t open_tag);
void zstream_reject(uint32_t consumer_pid, uint32_t open_tag, const char *error);

// feed each message your own loop reads to this. if it's a
// stream-protocol message for *this* stream (matched by stream_id,
// pulled from the message's tag), handles the bookkeeping (freeing
// the previous chunk on a fresh pull, matching a retry against the
// last chunk instead) and returns the event for you to act on.
// otherwise returns ZSTREAM_EVENT_NONE and leaves the message alone
// for you to keep handling.
zstream_event_t zstream_producer_handle(zstream_producer_t *st, z_msg_t *msg);

// reply to the pull that zstream_producer_handle() just reported.
// exactly one of these should be called per ZSTREAM_EVENT_PULL.
void zstream_send_chunk(zstream_producer_t *st, const void *data, uint32_t len);
void zstream_send_eof(zstream_producer_t *st);
void zstream_send_error(zstream_producer_t *st, const char *msg);

// release any held chunk and mark the stream inactive. call after
// EOF/error/abort, or if you're giving up on the stream yourself.
void zstream_producer_close(zstream_producer_t *st);

// -- consumer side --
//
// blocking, for a consumer that has nothing else to do while it
// waits (matching this codebase's existing tget/tput style). built
// directly on z_msg_send()/z_msg_read()/z_uptime_ticks(), so it works
// unmodified from either an app or the kernel, resolving at link
// time to whichever implementation (zeitlos.o/msg.o) is actually
// linked in.

typedef struct {

	uint32_t	stream_id;
	uint32_t	producer_pid;
	uint32_t	seq;
	bool		active;
	uint32_t	open_tag;	// used only while opening (before
					// stream_id exists) to match
					// OPEN_REPLY -- see zstream_open_async()

} zstream_consumer_t;

// open_payload is forwarded to the producer as-is -- zstream doesn't
// interpret it. returns true and fills *st on success; on failure
// returns false and fills err (if non-NULL) with the producer's
// error message. times out (a few seconds) if the producer never
// replies at all.
bool zstream_open(zstream_consumer_t *st, uint32_t producer_pid,
	z_obj_t open_payload, char *err, uint32_t err_len);

// blocking pull of the next chunk. on ZSTREAM_CHUNK, *data/*len point
// into the message's own borrowed storage -- per zmsg.h's usual
// borrowing rule, valid only until your next zstream_pull() call (or
// any other message send/read), so copy it out (e.g. write it to
// disk) before pulling again. on ZSTREAM_ERROR, err (if non-NULL) is
// filled with the producer's message. also times out if the producer
// never replies.
zstream_result_t zstream_pull(zstream_consumer_t *st, const uint8_t **data,
	uint32_t *len, char *err, uint32_t err_len);

// tell the producer to stop and clean up. does not block for a reply
// -- there isn't one; this is fire-and-forget by design, so a
// consumer giving up doesn't have to wait on a producer that may
// itself be stuck.
void zstream_abort(zstream_consumer_t *st);

// -- consumer side, non-blocking --
//
// for a consumer that -- unlike the shell -- can't block waiting on
// its producer, because it has its own other work to keep servicing
// in the meantime (e.g. `net` acting as the consumer for a TFTP PUT:
// it's pulling local file data from whoever asked for the PUT, while
// simultaneously still needing to service the actual TFTP protocol
// exchange with the remote server). same "fed messages the caller's
// own loop already read" shape as the producer side above, for the
// same reason.

typedef enum {
	ZSTREAM_CEVENT_NONE = 0,	// msg wasn't relevant to this consumer
	ZSTREAM_CEVENT_OPENED,		// open succeeded -- stream_id now set,
					// call zstream_pull_async() to start pulling
	ZSTREAM_CEVENT_OPEN_FAILED,	// open failed; err filled
	ZSTREAM_CEVENT_CHUNK,		// got a chunk; *data/*len point into
					// borrowed storage -- same lifetime rule
					// as zstream_pull(): valid only until your
					// next zstream_pull_async() call
	ZSTREAM_CEVENT_EOF,		// producer says done, no more chunks
	ZSTREAM_CEVENT_ERROR		// producer says error; err filled
} zstream_consumer_event_t;

// fire off the OPEN message and return immediately. watch for
// ZSTREAM_CEVENT_OPENED/OPEN_FAILED from zstream_consumer_handle().
void zstream_open_async(zstream_consumer_t *st, uint32_t producer_pid,
	z_obj_t open_payload);

// fire off a PULL for the next seq and return immediately. call once
// after ZSTREAM_CEVENT_OPENED, and again after each ZSTREAM_CEVENT_CHUNK
// is fully handled -- same one-outstanding-pull discipline as the rest
// of this protocol; calling it again before the previous pull's reply
// arrives isn't meaningful (there's nothing pending to correlate a
// second reply against).
void zstream_pull_async(zstream_consumer_t *st);

// feed each message your own loop reads to this, exactly like
// zstream_producer_handle() on the other side.
zstream_consumer_event_t zstream_consumer_handle(zstream_consumer_t *st,
	z_msg_t *msg, const uint8_t **data, uint32_t *len,
	char *err, uint32_t err_len);

#endif
