/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Streaming layer on top of messaging. See zstream.h.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "zstream.h"
#include "zobj.h"
#include "zmsg.h"

// deliberately NOT #include "zeitlos.h" or "msg.h" -- either would
// work at compile time, but zeitlos.h also declares getch()/
// readline()/echo()/noecho(), which collide with kruntime.c's own
// definitions in the kernel build (the same reason sh.c has its own
// separate msg.c instead of linking zeitlos.c). declaring just the
// four functions this file actually needs keeps it buildable into
// either an app (linking zeitlos.o) or the kernel (linking msg.o)
// unmodified -- both provide matching signatures (see msg.h's
// comment on z_msg_send for why).
z_rv z_msg_send(z_msg_t *msg);
z_rv z_msg_read(z_msg_t *msg);
z_rv z_msg_new_send(uint32_t to, uint32_t subject, uint32_t tag, z_obj_t obj);
uint32_t z_uptime_ticks(void);

#define ZSTREAM_TIMEOUT_TICKS (3 * 732)	// ~3s -- generous for
	// in-memory messaging plus a slow consumer-side operation (e.g.
	// an SD card write); short enough that a genuinely stuck peer
	// is noticed quickly. applies to the consumer side's blocking
	// calls only -- the producer side never blocks at all.

static void set_err(char *err, uint32_t err_len, const char *msg) {
	if (!err || !err_len) return;
	uint32_t i = 0;
	for (; msg[i] && i < err_len - 1; i++) err[i] = msg[i];
	err[i] = 0;
}

// -- producer side --

static uint32_t next_stream_id = 1;	// 0 reserved as "no stream" (see
					// zstream_reject())

static uint32_t next_seq(zstream_producer_t *st) {
	return (st->seq == UINT32_MAX) ? 0 : (st->seq + 1);
}

void zstream_accept(zstream_producer_t *st, uint32_t consumer_pid, uint32_t open_tag) {

	memset(st, 0, sizeof(*st));
	st->stream_id = next_stream_id++;
	if (next_stream_id == 0) next_stream_id = 1;	// skip 0 on wrap
	st->consumer_pid = consumer_pid;
	st->seq = UINT32_MAX;		// no chunk sent yet
	st->active = true;
	st->last_chunk = z_obj_none();

	z_msg_new_send(consumer_pid, Z_STREAM_OPEN_REPLY, open_tag,
		z_obj_uint32(st->stream_id));

}

void zstream_reject(uint32_t consumer_pid, uint32_t open_tag, const char *error) {
	// one-shot message, same pattern (and same small, one-time, never
	// repeated cost) as the existing TFTP reply in net.c -- not
	// freed here since the consumer hasn't necessarily read it yet
	// by the time z_msg_new_send() returns (see zmsg.h's borrowing
	// rule). fine for something that happens at most once per
	// rejected open, unlike a per-chunk send.
	z_msg_new_send(consumer_pid, Z_STREAM_OPEN_REPLY, open_tag, z_obj_str(error));
}

zstream_event_t zstream_producer_handle(zstream_producer_t *st, z_msg_t *msg) {

	if (!st->active) return ZSTREAM_EVENT_NONE;

	uint32_t stream_id = ZSTREAM_TAG_STREAM_ID(msg->tag);
	if (stream_id != (st->stream_id & 0xffff)) return ZSTREAM_EVENT_NONE;

	if (msg->subject == Z_STREAM_ABORT)
		return ZSTREAM_EVENT_ABORT;

	if (msg->subject != Z_STREAM_PULL) return ZSTREAM_EVENT_NONE;

	uint32_t seq = ZSTREAM_TAG_SEQ(msg->tag);
	uint32_t expect_next = next_seq(st);

	if (seq == expect_next) {
		// fresh pull -- this arriving is itself proof the consumer
		// already has the chunk we sent last time, so it's safe to
		// free now (see zstream.h's big comment on why this is the
		// only place a chunk ever gets freed)
		if (st->last_chunk.type == Z_BLOB) z_obj_free(&st->last_chunk);
		st->last_chunk = z_obj_none();
		return ZSTREAM_EVENT_PULL;
	}

	if (st->seq != UINT32_MAX && seq == st->seq) {
		// retry of the chunk we just sent -- still have it, resend
		// directly rather than bothering the caller for data it may
		// not have handy anymore
		z_msg_new_send(st->consumer_pid, Z_STREAM_CHUNK,
			ZSTREAM_TAG(st->stream_id, st->seq), st->last_chunk);
		return ZSTREAM_EVENT_NONE;
	}

	// stale/invalid seq -- ignore
	return ZSTREAM_EVENT_NONE;

}

void zstream_send_chunk(zstream_producer_t *st, const void *data, uint32_t len) {
	uint32_t seq = next_seq(st);
	z_obj_t chunk = z_obj_blob(data, len);
	z_msg_new_send(st->consumer_pid, Z_STREAM_CHUNK,
		ZSTREAM_TAG(st->stream_id, seq), chunk);
	st->last_chunk = chunk;	// kept alive until the next pull frees it
	st->seq = seq;
}

void zstream_send_eof(zstream_producer_t *st) {
	if (st->last_chunk.type == Z_BLOB) z_obj_free(&st->last_chunk);
	st->last_chunk = z_obj_none();
	uint32_t seq = next_seq(st);
	z_msg_new_send(st->consumer_pid, Z_STREAM_EOF,
		ZSTREAM_TAG(st->stream_id, seq), z_obj_none());
	st->seq = seq;
}

void zstream_send_error(zstream_producer_t *st, const char *msg) {
	if (st->last_chunk.type == Z_BLOB) z_obj_free(&st->last_chunk);
	st->last_chunk = z_obj_none();
	uint32_t seq = next_seq(st);
	// terminal, one-shot message for this stream -- same small,
	// one-time cost as zstream_reject() above, never repeated.
	z_msg_new_send(st->consumer_pid, Z_STREAM_ERROR,
		ZSTREAM_TAG(st->stream_id, seq), z_obj_str(msg));
	st->seq = seq;
}

void zstream_producer_close(zstream_producer_t *st) {
	if (st->last_chunk.type == Z_BLOB) z_obj_free(&st->last_chunk);
	st->active = false;
}

// -- consumer side --

bool zstream_open(zstream_consumer_t *st, uint32_t producer_pid,
	z_obj_t open_payload, char *err, uint32_t err_len) {

	memset(st, 0, sizeof(*st));
	st->producer_pid = producer_pid;

	static uint32_t next_open_tag = 1;
	uint32_t open_tag = next_open_tag++;
	if (next_open_tag == 0) next_open_tag = 1;

	z_msg_new_send(producer_pid, Z_STREAM_OPEN, open_tag, open_payload);

	uint32_t start = z_uptime_ticks();
	z_msg_t reply;

	while (z_uptime_ticks() - start < ZSTREAM_TIMEOUT_TICKS) {

		if (z_msg_read(&reply) != Z_OK) continue;
		if (reply.subject != Z_STREAM_OPEN_REPLY || reply.tag != open_tag)
			continue;	// not our reply -- discard, keep waiting

		if (reply.obj.type == Z_UINT32) {
			st->stream_id = reply.obj.val.uint32;
			st->seq = 0;	// next seq we'll pull
			st->active = true;
			return true;
		}

		if (reply.obj.type == Z_STR && reply.obj.val.str) {
			set_err(err, err_len, reply.obj.val.str);
			return false;
		}

		set_err(err, err_len, "malformed open reply");
		return false;

	}

	set_err(err, err_len, "timed out waiting to open stream");
	return false;

}

zstream_result_t zstream_pull(zstream_consumer_t *st, const uint8_t **data,
	uint32_t *len, char *err, uint32_t err_len) {

	if (!st->active) {
		set_err(err, err_len, "stream not open");
		return ZSTREAM_ERROR;
	}

	uint32_t tag = ZSTREAM_TAG(st->stream_id, st->seq);
	z_msg_new_send(st->producer_pid, Z_STREAM_PULL, tag, z_obj_none());

	uint32_t start = z_uptime_ticks();
	z_msg_t msg;

	while (z_uptime_ticks() - start < ZSTREAM_TIMEOUT_TICKS) {

		if (z_msg_read(&msg) != Z_OK) continue;
		if (msg.tag != tag) continue;	// not for this pull -- discard

		if (msg.subject == Z_STREAM_CHUNK) {
			if (msg.obj.type != Z_BLOB) {
				st->active = false;
				set_err(err, err_len, "malformed chunk (not a blob)");
				return ZSTREAM_ERROR;
			}
			if (data) *data = z_blob_data(&msg.obj);
			if (len) *len = z_blob_len(&msg.obj);
			st->seq++;
			return ZSTREAM_CHUNK;
		}

		if (msg.subject == Z_STREAM_EOF) {
			st->active = false;
			return ZSTREAM_EOF;
		}

		if (msg.subject == Z_STREAM_ERROR) {
			st->active = false;
			if (msg.obj.type == Z_STR && msg.obj.val.str)
				set_err(err, err_len, msg.obj.val.str);
			else
				set_err(err, err_len, "producer reported an error");
			return ZSTREAM_ERROR;
		}

		// anything else matching this tag shouldn't happen -- ignore,
		// keep waiting rather than treat it as fatal

	}

	st->active = false;
	set_err(err, err_len, "timed out waiting for next chunk");
	return ZSTREAM_ERROR;

}

void zstream_abort(zstream_consumer_t *st) {
	if (!st->active) return;
	uint32_t tag = ZSTREAM_TAG(st->stream_id, st->seq);
	z_msg_new_send(st->producer_pid, Z_STREAM_ABORT, tag, z_obj_none());
	st->active = false;
}

// -- consumer side, non-blocking --

void zstream_open_async(zstream_consumer_t *st, uint32_t producer_pid,
	z_obj_t open_payload) {

	memset(st, 0, sizeof(*st));
	st->producer_pid = producer_pid;

	static uint32_t next_open_tag = 1;
	st->open_tag = next_open_tag++;
	if (next_open_tag == 0) next_open_tag = 1;

	z_msg_new_send(producer_pid, Z_STREAM_OPEN, st->open_tag, open_payload);

}

void zstream_pull_async(zstream_consumer_t *st) {
	if (!st->active) return;
	uint32_t tag = ZSTREAM_TAG(st->stream_id, st->seq);
	z_msg_new_send(st->producer_pid, Z_STREAM_PULL, tag, z_obj_none());
}

zstream_consumer_event_t zstream_consumer_handle(zstream_consumer_t *st,
	z_msg_t *msg, const uint8_t **data, uint32_t *len,
	char *err, uint32_t err_len) {

	// still opening -- match on open_tag, since stream_id doesn't
	// exist yet
	if (!st->active && st->stream_id == 0) {

		if (msg->subject != Z_STREAM_OPEN_REPLY || msg->tag != st->open_tag)
			return ZSTREAM_CEVENT_NONE;

		if (msg->obj.type == Z_UINT32) {
			st->stream_id = msg->obj.val.uint32;
			st->seq = 0;
			st->active = true;
			return ZSTREAM_CEVENT_OPENED;
		}

		if (msg->obj.type == Z_STR && msg->obj.val.str) {
			set_err(err, err_len, msg->obj.val.str);
			return ZSTREAM_CEVENT_OPEN_FAILED;
		}

		set_err(err, err_len, "malformed open reply");
		return ZSTREAM_CEVENT_OPEN_FAILED;

	}

	if (!st->active) return ZSTREAM_CEVENT_NONE;

	uint32_t tag = ZSTREAM_TAG(st->stream_id, st->seq);
	if (msg->tag != tag) return ZSTREAM_CEVENT_NONE;	// not our pull's reply

	if (msg->subject == Z_STREAM_CHUNK) {
		if (msg->obj.type != Z_BLOB) {
			st->active = false;
			set_err(err, err_len, "malformed chunk (not a blob)");
			return ZSTREAM_CEVENT_ERROR;
		}
		if (data) *data = z_blob_data(&msg->obj);
		if (len) *len = z_blob_len(&msg->obj);
		st->seq++;
		return ZSTREAM_CEVENT_CHUNK;
	}

	if (msg->subject == Z_STREAM_EOF) {
		st->active = false;
		return ZSTREAM_CEVENT_EOF;
	}

	if (msg->subject == Z_STREAM_ERROR) {
		st->active = false;
		if (msg->obj.type == Z_STR && msg->obj.val.str)
			set_err(err, err_len, msg->obj.val.str);
		else
			set_err(err, err_len, "producer reported an error");
		return ZSTREAM_CEVENT_ERROR;
	}

	return ZSTREAM_CEVENT_NONE;

}
