#ifndef ZMSG_H
#define ZMSG_H

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Inter-process messaging.
 *
 * Zeitlos processes don't have separate physical memory -- every
 * process's memory is reachable by everyone, the MTU just relocates
 * each process's own view of it to a fixed address. That means a
 * message payload doesn't have to be copied to be handed to another
 * process: z_msg_send() queues a small envelope (to/from/subject/tag
 * + the top-level z_obj_t) into the kernel's own memory, and
 * z_msg_read() resolves any pointers it contains -- on demand, only
 * for the process that actually reads the message.
 *
 * Z_UINT32/INT32/FLOAT32/RETVAL/NONE payloads are plain values, so
 * there's nothing to resolve.
 *
 * Z_STR payloads are resolved to a physical pointer at the sender's
 * bytes -- no copy.
 *
 * Z_LIST/Z_MAP payloads have their *structural* nodes (the
 * z_obj_table_t headers and the z_obj_t slots inside them) rebuilt
 * into scratch space embedded in the receiver's own z_msg_t -- again,
 * no copy of the leaf data (numbers, string bytes). This is what
 * Z_MSG_MAX_TABLES/Z_MSG_MAX_ITEMS below are for.
 *
 * Z_BLOB payloads (arbitrary-length binary data -- see zobj.h) work
 * like Z_STR: the actual bytes are never copied, only pointer-
 * translated. Since a blob is an indirect {len, data} header (not a
 * single pointer like Z_STR), the header itself gets rebuilt into a
 * small scratch pool (Z_MSG_MAX_BLOBS below), same idea as the
 * table/item pools for lists/maps.
 *
 * In every case, the data is BORROWED from the sender's memory. It's
 * only guaranteed to stay valid until you send your own next message
 * (which may let the sender run again and reuse or free it). Never
 * call z_obj_free() on anything reached through msg.obj. If you need
 * to keep or modify the data, make your own copy first with
 * z_obj_copy() -- that allocates fresh memory on your own heap, same
 * as it does for any other z_obj_t.
 */

#include <stdint.h>
#include "zobj.h"

// generic kernel/process return value
typedef uint32_t z_rv;
#define Z_OK   0
#define Z_FAIL 1

// how many pending messages a single process's mailbox can hold.
// small on purpose -- messages are meant to be drained promptly, this
// isn't a general-purpose queue.
#define Z_MAILBOX_DEPTH   8

// scratch budget used by z_msg_read() to resolve Z_LIST/Z_MAP
// payloads (see the big comment above). enough for a small, flat
// argument map; raise these if you need more.
#define Z_MSG_MAX_TABLES   4
#define Z_MSG_MAX_ITEMS    16

// scratch budget for Z_BLOB payload headers (see above). small on
// purpose -- a message typically carries at most one blob (e.g. one
// UDP packet's worth of data), maybe two if nested in a small map.
#define Z_MSG_MAX_BLOBS    2

// a message as seen by a process.
typedef struct {

	uint32_t	to;
	uint32_t	from;		// stamped by the kernel on send; ignored if set by the caller
	uint32_t	subject;
	uint32_t	tag;		// useful for matching RPC replies
	z_obj_t		obj;

	// scratch space used only when obj (or something inside it) is a
	// Z_LIST/Z_MAP/Z_BLOB -- see above.
	z_obj_table_t	_tables[Z_MSG_MAX_TABLES];
	z_obj_t		_items[Z_MSG_MAX_ITEMS];
	z_blob_t	_blobs[Z_MSG_MAX_BLOBS];

} z_msg_t;

// the lightweight envelope actually queued in a process's kernel-owned
// mailbox. payload pointers are still expressed in the sender's own
// address space at this point -- z_msg_read() resolves them into the
// reading process's z_msg_t at read time.
typedef struct {

	uint32_t	to;
	uint32_t	from;
	uint32_t	subject;
	uint32_t	tag;
	z_obj_t		obj;

} z_msg_envelope_t;

#endif
