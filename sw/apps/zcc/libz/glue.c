/*
 * libz -- the parts that must speak sw/common's own types.
 *
 * Everything else in this runtime is declared by libz.h, which is
 * deliberately self-contained: a program can include it and nothing
 * else. That does not work for messaging. `z_msg_send()` takes a
 * `z_msg_t`, sw/common/zwin.c calls it with one, and a `void *`
 * version declared alongside sw/common/zeitlos.h's real one is a
 * conflicting declaration, not a compatible simplification.
 *
 * So this file is compiled against zeitlos.h -- with the shim headers,
 * not libz.h -- and supplies the messaging entry points with their
 * real signatures. zcc resolves a call to them from the tree's own
 * headers, which is what an app that opens a window includes anyway.
 *
 * z_msg_new_send() and z_msg_wait() are here for a second reason: they
 * live in sw/common/zeitlos.c, which cannot go into this runtime at
 * all (it defines newlib's _read/_write/_sbrk and would drag a libc in
 * behind it). They are four lines each and zwin.c needs both.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "zeitlos.h"
#include "zobj.h"
#include "zmsg.h"

typedef struct { int type; unsigned val; } zobj_raw_t;

extern void *z_syscall(unsigned id, void *arg);

#define ZS_MSG_SEND 7
#define ZS_MSG_READ 8

static z_rv syscall_rv(unsigned id, void *arg) {
    zobj_raw_t *rv = (zobj_raw_t *)z_syscall(id, arg);
    return rv ? (z_rv)rv->val : Z_FAIL;
}

z_rv z_msg_send(z_msg_t *msg) { return syscall_rv(ZS_MSG_SEND, msg); }
z_rv z_msg_read(z_msg_t *msg) { return syscall_rv(ZS_MSG_READ, msg); }

z_rv z_msg_new_send(uint32_t to, uint32_t subject, uint32_t tag, z_obj_t obj) {
    z_msg_t msg;
    msg.to = to;
    msg.subject = subject;
    msg.tag = tag;
    msg.obj = obj;
    return z_msg_send(&msg);
}

/*
 * Blocks until the matching message arrives, discarding anything else.
 *
 * Same shape as sw/common/zeitlos.c's version, and it inherits the
 * same caveat: non-matching messages are DROPPED, not requeued. That
 * is fine for a request/reply exchange where the caller is the only
 * thing talking to that peer, and it is why nothing here uses it as a
 * general receive loop.
 */
z_rv z_msg_wait(z_msg_t *msg, uint32_t subject, uint32_t tag) {
    for (;;) {
        if (z_msg_read(msg) == Z_OK) {
            if (msg->subject == subject && msg->tag == tag) return Z_OK;
        }
    }
}

/*
 * Font accessors.
 *
 * The fonts (sw/common/zfont_data.c) are const DATA, not functions, so
 * they cannot be jump-table entries -- a slot holds an instruction. A
 * zcc-compiled program has no way to take the address of a global
 * inside the blob, because the blob exports functions and nothing
 * else.
 *
 * An accessor per font is the smallest fix and the one with no new
 * mechanism behind it: four table slots, each returning a pointer.
 * The alternative -- a second, data-shaped export table that zcc
 * resolves address references against -- would be more general and is
 * not worth building until something needs it that is not a font.
 */

#include "zfont.h"

const z_font_t *z_font_8x16_get(void) { return &z_font_8x16; }
const z_font_t *z_font_6x12_get(void) { return &z_font_6x12; }
const z_font_t *z_font_5x7_get(void)  { return &z_font_5x7; }
const z_font_t *z_font_5x8_get(void)  { return &z_font_5x8; }
