# Zeitlos Object & Messaging System Developer Guide

## Overview

Zeitlos processes talk to each other by sending small, typed messages
through kernel-owned mailboxes. There is no shared-memory API and no
pipes -- messaging is the one mechanism apps use to ask the kernel,
the window manager, or each other to do things.

This guide covers two layers:

- **The object system** (`z_obj_t`, `sw/common/zobj.c/h`) -- a small
  tagged-union value type used both as general-purpose data and as
  message payloads.
- **The messaging system** (`z_msg_t`, `sw/common/zmsg.h`,
  `sw/os/msg.c`, and the `z_msg_*` functions in `sw/common/zeitlos.c`)
  -- mailboxes, the `z_msg_send`/`z_msg_read`/`z_msg_wait` API, and
  the rules around what you can safely do with a message once you
  have one.

See `docs/app_runtime.md` for where these `z_msg_*` wrappers sit
within the broader app runtime (the syscall trampoline they're built
on, direct hardware register access, `zgfx.c`).

## The object system

### Types

| Type | Holds | Notes |
|---|---|---|
| `Z_NONE` | nothing | default/empty value |
| `Z_RETVAL` | `int32` | 0 = ok, non-zero = fail; used internally for syscall return values (`z_ok`/`z_fail`) |
| `Z_UINT32` | `uint32_t` | |
| `Z_INT32` | `int32_t` | |
| `Z_FLOAT32` | `float` | |
| `Z_STR` | `char *` | heap-allocated, NUL-terminated |
| `Z_LIST` | `z_obj_table_t *` | ordered array of `z_obj_t` |
| `Z_MAP` | `z_obj_table_t *` | parallel key/value arrays of `z_obj_t` (keys are conventionally `Z_STR`) |

Every `z_obj_t` is a small value (`type` + 4-byte union) that gets
passed and returned **by value**, not by pointer -- e.g.
`z_obj_t z_obj_uint32(uint32_t u)` returns a complete object, it
doesn't allocate one for you to free later. Only `Z_STR`, `Z_LIST` and
`Z_MAP` actually own heap memory (a string buffer, or a
`z_obj_table_t` with its `a`/`b` arrays).

### Creating objects

```c
z_obj_t z_obj_none(void);
z_obj_t z_obj_uint32(uint32_t u);
z_obj_t z_obj_int32(int32_t i);       // alias: z_obj_int
z_obj_t z_obj_float32(float f);       // alias: z_obj_float
z_obj_t z_obj_str(const char *s);     // copies s into a new heap buffer
z_obj_t z_obj_list(uint32_t len);     // fixed-length, len slots start as Z_NONE
z_obj_t z_obj_map(uint32_t len);      // fixed-length, len key/value slots
```

`z_obj_list`/`z_obj_map` are fixed-capacity once created (there's no
grow-on-append). `z_list_append`/`z_map_set` fill the first available
`Z_NONE` slot rather than resizing the table, so size the table for
how many entries you expect up front.

### Reading, copying, freeing

```c
z_obj_t *z_list_get(z_obj_t *obj, uint32_t index);
z_obj_t *z_map_get_key(z_obj_t *obj, uint32_t index);
z_obj_t *z_map_get_val(z_obj_t *obj, uint32_t index);
z_obj_t *z_map_find(z_obj_t *map, const char *key);   // linear search by key

z_obj_t z_obj_copy(const z_obj_t *src);   // deep copy, allocates fresh memory
int z_obj_equal(const z_obj_t *a, const z_obj_t *b);
uint32_t z_obj_size(const z_obj_t *obj);  // element count (list/map) or strlen (str)

void z_obj_free(z_obj_t *obj);  // recursively frees Z_STR/Z_LIST/Z_MAP; safe on anything else
```

`z_obj_free()` always recurses through lists/maps and frees every
string, table, and array it owns, then resets the object to
`Z_NONE`. **Ownership matters**: only free an object you (or
`z_obj_copy()`) actually allocated. See the messaging section below
for why this is a hard rule for message payloads specifically.

## The messaging system

### Why messages aren't serialized

Zeitlos doesn't isolate process memory the way a paged-MMU OS does --
it's a genuinely flat physical address space, and the MTU just
relocates each process's own view of memory to a fixed virtual
address (`0x8000_0000`). Every process's heap lives inside one
contiguous block of physical memory (`z_procs[pid].base`), and
addresses below the MTU's mirror window aren't translated at all --
they always refer to physical memory, from any process's context.

That means a pointer created by process A can be converted to the
*physical* address it refers to with one calculation
(`vaddr - 0x8000_0000 + A.base`), and that physical address is then
directly readable by anyone, kernel or app, without copying the data
it points to.

So instead of serializing message payloads into a byte buffer,
Zeitlos passes `Z_STR`/`Z_LIST`/`Z_MAP` payloads **by reference**:

- Scalars (`Z_UINT32`/`INT32`/`FLOAT32`/`RETVAL`/`NONE`) are copied by
  value -- there's nothing to resolve, this is effectively free.
- `Z_STR` payloads are resolved to a physical pointer at the sender's
  actual bytes. No copy of the string data happens.
- `Z_LIST`/`Z_MAP` payloads have their *structural* nodes (the
  `z_obj_table_t` headers and the `z_obj_t` slots inside them)
  rebuilt into scratch space inside the receiver's own `z_msg_t`; the
  leaf data (numbers, string bytes) is still never copied.

This is much cheaper than a general serialize/deserialize pass,
which matters on a 48MHz core where messaging is meant to be a
central, high-frequency mechanism (window moves, input events, RPC
calls) rather than an occasional bulk-transfer operation.

### The cost: borrowed data has a lifetime

Because the payload isn't copied, **the sender's memory has to stay
valid for as long as the receiver might read it.** If the sender
frees or overwrites the underlying data, or exits, the receiver ends
up reading garbage -- there's no MMU to fault on that.

The rule this implies:

> **Non-scalar payloads (`Z_STR`/`Z_LIST`/`Z_MAP`) are borrowed, and
> only safe for synchronous request/reply exchanges.** The sender
> should not touch or free the object until it's received a reply (or
> otherwise knows the receiver is done with it). The receiver must
> treat the payload as read-only and must never call `z_obj_free()`
> on it. If the receiver needs to keep or modify the data past the
> point where it might send its own next message, it should
> `z_obj_copy()` it first -- that allocates real memory on the
> receiver's own heap, same as any other `z_obj_t`.

In practice this means: reply-with-a-string/list/map patterns (like
`pong` replying to `ping`, or the window manager replying to a
`Z_WM_CREATE_WINDOW` request) are the intended use, and they're safe
as long as the pattern stays a strict request-then-reply. Fire-and-
forget messages should stick to scalar payloads.

### `z_msg_t`

```c
typedef struct {
    uint32_t to;
    uint32_t from;      // stamped by the kernel on send; a caller-set value is ignored
    uint32_t subject;    // what kind of message this is (app-defined constants)
    uint32_t tag;        // useful for matching RPC replies to requests
    z_obj_t  obj;         // the payload
    z_obj_table_t _tables[Z_MSG_MAX_TABLES];  // scratch space, see above
    z_obj_t       _items[Z_MSG_MAX_ITEMS];    // scratch space, see above
} z_msg_t;
```

`_tables`/`_items` are internal scratch space that `z_msg_read()` uses
to rebuild `Z_LIST`/`Z_MAP` payloads -- don't touch them directly,
they exist so the resolved payload has somewhere to live inside your
own `z_msg_t`. Current limits (`sw/common/zmsg.h`):

| Constant | Default | Meaning |
|---|---|---|
| `Z_MAILBOX_DEPTH` | 8 | pending messages a process's mailbox can hold before `z_msg_send()` starts failing |
| `Z_MSG_MAX_TABLES` | 4 | `z_obj_table_t` nodes (list/map instances) a single message payload can reference |
| `Z_MSG_MAX_ITEMS` | 16 | total `z_obj_t` slots across all of a payload's table arrays |

These are static/compile-time (no dynamic allocation in the kernel
for messaging) -- raise them if you need to pass bigger argument
maps. If a payload doesn't fit the budget, `z_msg_read()` still pops
the message (so the mailbox doesn't get stuck) but resets `obj` to
`Z_NONE`.

### API

```c
z_rv z_msg_send(z_msg_t *msg);
```
Sends a pre-built message. Fails if `to` isn't a live process or its
mailbox is full.

```c
z_rv z_msg_new_send(uint32_t to, uint32_t subject, uint32_t tag, z_obj_t obj);
```
Convenience wrapper -- builds a `z_msg_t` from the given fields and
sends it. This is a pure runtime helper (`sw/common/zeitlos.c`), not a
separate syscall.

```c
z_rv z_msg_read(z_msg_t *msg);
```
Pops the next available message from the calling process's mailbox
into `msg`, resolving any borrowed payload along the way. Returns
`Z_FAIL` (leaving `msg` untouched) if the mailbox is empty --
non-blocking.

```c
z_rv z_msg_wait(z_msg_t *msg, uint32_t subject, uint32_t tag);
```
Blocks (busy-loops -- there's no yield/sleep primitive yet, so this
relies on preemptive scheduling to let other processes run) until a
message matching both `subject` and `tag` arrives, discarding
anything else that shows up in the meantime. This is a runtime-only
loop over `z_msg_read()`, not a separate syscall.

### Subjects and tags

`subject` and `tag` are both just `uint32_t` -- Zeitlos doesn't impose
a global registry. Convention so far:

- `subject` identifies the *kind* of message (e.g. "this is a ping",
  "this is a create-window request"). Each subsystem defines its own
  constants (see `Z_WM_*` in `sw/common/zwm.h` for the window
  manager's).
- `tag` is available for matching a reply to a specific outstanding
  request when a process might have more than one in flight. The
  ping/pong and window-manager demos don't need it yet (each process
  only ever has one request outstanding at a time) and use `0`
  throughout, but a real RPC client with multiple concurrent requests
  would pick a fresh tag per request and match on it.

### Kernel internals, briefly

Mailboxes live in kernel-owned static memory (`sw/os/msg.c`), one
fixed-depth ring buffer per process slot -- not inside any process's
own memory. `z_msg_send()` (syscall handler `k_msg_send`) copies a
small envelope (`to`/`from`/`subject`/`tag`/`obj`) into the
destination's ring; any `Z_STR`/`Z_LIST`/`Z_MAP` pointer in `obj` is
left exactly as the sender wrote it (still expressed in the sender's
own address space) -- nothing is resolved at send time.

`z_msg_read()` (syscall handler `k_msg_read`) pops the next envelope
and only then walks the payload (`z_resolve_obj()` in `msg.c`),
converting sender-relative pointers to physical addresses and
rebuilding list/map structure into the caller's `z_msg_t` scratch
arrays. This is deliberately lazy: a message that's sent but never
read never pays the resolution cost, and the common case (a scalar
payload) never pays it either since there's nothing to resolve.

Mailbox push/pop briefly mask IRQs (`maskirq()`) around the ring
buffer update, since the timer IRQ can preempt a process mid-update
and let a different process touch the same mailbox concurrently.

## Example: ping/pong

`sw/apps/ping` and `sw/apps/pong` (see their source for the full
listing) demonstrate the whole flow:

- `pong` blocks on `z_msg_wait(&msg, MSG_PING, 0)`, reads a `Z_UINT32`
  counter out of `msg.obj`, and replies with a `Z_STR` built via
  `z_obj_str()` -- a real heap allocation in pong's own memory, so it
  stays valid for ping to read.
- `ping` blocks on `z_msg_wait(&msg, MSG_PONG, 0)`, reads
  `msg.obj.val.str` directly (borrowed from pong's memory -- safe
  because ping does nothing else with its own messaging before it's
  done reading), then sends its own next ping.

Because the two apps strictly alternate (pong never sends a second
reply until it's received another ping, which can't happen until ping
has already read the previous reply), the borrowed-string lifetime
rule above is satisfied automatically. Both apps rely on `msg.from` --
stamped by the kernel, not the sender -- so pong doesn't need to know
ping's pid in advance.

Note: pong never frees the strings it allocates each reply (a small,
documented, intentional leak for this simple demo -- freeing
immediately after `z_msg_send()` would race with ping still reading
it, and there's no ack mechanism yet to know when it's safe). See
"Known limitations" below.

## Known limitations / future work

- **No reply-lifetime ack.** A sender of a borrowed payload has no
  way to know when the receiver is actually done reading it beyond
  "the reply-then-request pattern happens to make it safe." Anything
  that needs to free reply memory promptly (rather than leak it, or
  rely on request/reply alternation) needs a real mechanism here.
- **No process-death notification.** If a process holding a reference
  another process is relying on gets killed, there's currently no
  message or callback that tells anyone. This matters most for the
  window manager (an app's window should probably get cleaned up if
  the owning app dies) -- see `docs/window_manager.md`.
- **No dynamic pid discovery.** There's no name/role registry yet, so
  well-known processes (the window manager, and anything else that
  needs a stable identity) currently rely on being started at a
  specific, documented pid. See `docs/window_manager.md` for how this
  affects `Z_PID_WM`.
- **Fixed scratch/mailbox budgets.** `Z_MAILBOX_DEPTH`,
  `Z_MSG_MAX_TABLES`, `Z_MSG_MAX_ITEMS` are compile-time constants,
  not tunable per-process. Fine for control-message traffic; not
  meant for bulk data transfer -- for that, see `sw/common/zstream.h`,
  a pull-based streaming layer built on top of this messaging system
  specifically for moving data too large or too incremental for a
  single message (first used by TFTP, `docs/networking.md`, but
  generic). It also sidesteps the reply-lifetime problem above for
  its own case: a stream chunk's "when is it safe to free" question
  is answered by the next pull arriving, which is itself proof the
  previous chunk was received -- worth a look as a pattern even where
  streaming itself isn't the fit, since the same trick (let the next
  request double as an ack for the previous reply) could apply
  elsewhere.
