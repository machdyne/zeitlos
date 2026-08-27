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
it). `zport.h`'s DATA channel now has a real ack-based mechanism for
exactly this problem (see "Known limitations" below) -- ping/pong
itself doesn't use `zport.h`, so this doesn't change here, but the
same trick would apply if this demo ever needed to stop leaking.

## Known limitations / future work

- **No general reply-lifetime ack.** Outside of `zport.h`'s DATA
  channel (below), a sender of a borrowed payload still has no way to
  know when the receiver is actually done reading it beyond "the
  reply-then-request pattern happens to make it safe." One-shot
  RPC-style replies throughout this codebase (DHCP's ACK, DNS's reply,
  TFTP's per-request replies, `pong`'s own strings above) all still
  rely on exactly that pattern, or just leak intentionally -- and
  that's fine for them: one small allocation per REQUEST, not one per
  BYTE of an open-ended session, so the leak stays bounded by how many
  distinct requests get made, not by how long a connection stays open.
  **`zport.h`'s DATA channel is different, and now has a real fix --
  see below.**

  **Real-hardware finding, in three parts, the last one now
  resolved:** `sw/common/zport.h`'s `z_port_send()` originally just
  leaked its `z_obj_blob()` payload on every single call, following
  the same "small, accepted leak" precedent as `pong`'s reply strings
  -- but `zport.h`'s own usage pattern (potentially many sends per
  connection, e.g. one per keystroke echoed by `repl`, or one per
  chunk of telnet traffic relayed by `net`) isn't the same shape as a
  one-shot RPC reply, and confirmed on real hardware: enough sustained
  interactive use exhausted the sending process's own heap
  (`Z_PROC_STACK_SIZE`, `sw/os/kernel.c`), at which point
  `z_obj_blob()`'s internal `malloc()` started failing -- see that
  function's own comment (`sw/common/zobj.c`) for what an unchecked
  failure there used to do silently. A first attempted fix freed the
  PREVIOUS call's blob at the start of each new `z_port_send()`,
  reasoning that the peer must have had a scheduling slot to read it
  by then -- this is exactly the "no reply-lifetime ack" problem this
  bullet describes, and the fix turned out to demonstrate it rather
  than solve it: the assumption breaks whenever a caller makes several
  sends back-to-back with nothing in between to force a scheduler
  switch (confirmed on real hardware with `repl`'s own
  `handle_connect()`, which sends a banner then a prompt with no yield
  between them -- the second call's free ran before the peer had
  necessarily read the first message, and the very next
  `z_obj_blob()` call reused that just-freed memory, so the first
  message resolved to the SECOND message's bytes by the time the peer
  actually read it). Reverted -- `z_port_send()` went back to never
  freeing at all, relying on `Z_PROC_STACK_SIZE` to make the leak
  tolerable for a realistic session rather than a free that was
  actively incorrect. **Still not enough**: a genuinely long-running
  interactive session (a chatty telnet BBS, specifically) could still
  exhaust even the larger budget -- the leak was unbounded, just
  slower to hit.

  **The actual fix**: `zport.h` gained a real `Z_PORT_DATA_ACK`
  message. Whichever side receives a `Z_PORT_DATA` calls
  `z_port_send_ack()` once its own handler has GENUINELY finished
  reading the payload (not the moment `z_msg_read()` returns -- see
  below for why that distinction matters); the original sender's
  `z_port_send()` tracks each outstanding blob in a small, fixed-size
  per-connection FIFO queue (`Z_PORT_MAX_PENDING_SENDS`, `zport.h`) and
  frees the oldest entry once `z_port_handle_ack()` sees any ack
  arrive for that connection. Every current DATA sender/receiver
  (`net`'s telnet relay, `repl`, `term`, `portdemo`) now sends and
  handles this.

  **A second real-hardware finding, right after the first shipped:**
  the very first version of this matched an incoming ack against the
  pending entry by comparing the payload's own data POINTER -- and
  that never worked, because a pointer is only a meaningful, stable
  value in the process that allocated it. `sw/os/msg.c`'s own header
  comment spells out why: "a pointer created by process P is always
  `P.base + (vaddr - 0x80000000)` in physical terms." The receiver's
  `z_blob_data()` returns that already-resolved PHYSICAL address; the
  sender's own record of what it allocated (`b->data` from
  `z_obj_blob()`) is still expressed in the sender's OWN, unresolved,
  process-relative view -- a different numeric value, with no way for
  an app to redo that translation itself (`z_translate()` is
  kernel-internal). Every ack's "match" silently failed, every pending
  entry sat there forever unfreed, and the queue filled up after
  exactly `Z_PORT_MAX_PENDING_SENDS` sends -- observed on real hardware
  as `z_port_send()` starting to fail permanently (`Z_FAIL`, "echo
  z_port_send failed") after typing only a handful of characters into
  `term`. **Fixed by not matching on any transmitted value at all**:
  mailboxes are FIFO per sender/receiver pair, and every current DATA
  receiver acks exactly once, in the order it read each message -- so
  the Nth ack to arrive always corresponds to the Nth still-
  outstanding send, and popping the oldest entry off the queue on any
  ack is correct without needing to identify which one by value.

  **A third real-hardware finding, right after the second was fixed:**
  the FIFO-position approach above is only correct if every DATA
  message really does get exactly one ack, in order -- and the fix
  didn't yet guarantee the ACK ITSELF actually arrives, only that the
  DATA message did. `z_port_send_ack()` originally called
  `z_msg_new_send()` and discarded its return value, same class of
  omission `z_port_send()` itself used to have for the DATA send
  before it was fixed to free immediately on failure. But a lost ack
  is worse than a lost DATA message: a lost DATA message is caught and
  handled (see the "send that never gets delivered" bullet below); a
  lost ack silently and PERMANENTLY shifts the FIFO position mapping
  for every later ack on that connection, since there's no longer a
  1:1 correspondence between sends and acks to fall back on. First
  observed as a diagnostic length mismatch added specifically to catch
  this class of bug (the ack's own payload briefly carried the
  original message's length back as a cross-check) -- confirmed real
  on hardware: two adjacent entries (a 1-byte character echo, a
  3-byte backspace echo, `sw/common/zline.c`) showed up mismatched in
  exactly the pattern a one-position permanent desync produces. Traced
  to a receiving mailbox being transiently full right when `term`
  tried to send an ack back (the same class of keystroke-burst
  congestion `docs/ports.md`'s "Flow control" section already
  documents) -- **fixed by giving `z_port_send_ack()` a short, bounded
  retry** (`Z_PORT_ACK_RETRY_TICKS`, `zport.c`, ~0.5s) instead of
  either accepting the loss or trying to make the matching scheme
  robust to it. The diagnostic length check itself was removed once
  the real fix (reliable ack delivery) made it unnecessary -- keeping
  it around risked training people to read a length mismatch as
  meaningful when the actual invariant that matters is delivery, not
  content.

  A few design points worth keeping in mind, since they weren't
  obvious going in:
  - **The ack has to be sent by the receiving APPLICATION's own code,
    once it's actually done reading -- not automatically by the
    messaging layer at `z_msg_read()` time.** This system schedules
    preemptively (`sw/os/kernel.c`, KTIMER-driven round-robin), not
    cooperatively -- a receiver's own handler could in principle be
    interrupted mid-read. `z_msg_read()` only resolves the pointer; it
    doesn't mean the handler has actually read through the bytes yet.
    An automatic ack fired right at resolve time could let the sender
    free memory the receiver's own handler hasn't finished reading,
    reintroducing the exact corruption class described above.
    `term.c`'s `Z_PORT_DATA` handler, for example, sends the ack right
    after `vt_feed()` returns, not right after the `z_msg_read()` that
    produced the message.
  - **Every DATA receiver acks unconditionally, on every code path,
    even one that never actually touches the payload** -- a stale
    message against an already-closed connection, say. This isn't
    optional politeness: the FIFO-ordering guarantee above depends on
    every send eventually getting exactly one ack, in order, no matter
    what the receiver decided to do with it. Skipping an ack on some
    early-return path would misalign every later entry in that
    connection's queue, not just leave one thing unfreed.
  - **A receiving process still can't just free the memory itself,
    even though it has a real, dereferenceable pointer to it.** Each
    process's `malloc()`/`free()` (via `_sbrk()`, `zeitlos.c`/
    `kruntime.c`) is independent, per-process bump-allocator state --
    there's no shared heap across processes (that's a separate thing
    from `sw/os/mem.c`'s `k_mem_alloc()`, which only carves out whole
    *processes'* memory regions at creation time, not small in-process
    objects). A receiver calling `free()` on a pointer it didn't
    `malloc()` would be handing that address to a completely
    unrelated allocator instance, which could at best no-op/crash and
    at worst conclude that memory is now free and later hand it back
    out via one of the receiver's own ordinary `malloc()` calls --
    silent corruption of memory a different, still-live process
    considers its own. Only the original allocating process can safely
    free its own allocation, which is exactly why an ack back to that
    process, rather than a direct free by the receiver, is required.
  - **Backpressure is a deliberate side effect, not the goal.**
    `Z_PORT_MAX_PENDING_SENDS` being small and fixed means
    `z_port_send()` itself now refuses new sends (`Z_FAIL`) once a
    peer has fallen far enough behind on acking, rather than growing
    without bound. This gives `zport.h` a real memory ceiling it
    didn't have before, but it's not the general flow-control redesign
    `docs/ports.md`'s "Flow control: an explicit, deliberate gap for
    v1" describes as still-future work -- that section is about
    giving `zport` real pull/credit-based backpressure for its own
    sake (a large paste, a chatty remote); this is a narrow safety
    valve against the specific "peer stopped acking entirely" case,
    sized generously enough that ordinary interactive traffic never
    gets near it.
  - **A send that never gets delivered at all** (the peer's mailbox is
    full, `z_msg_send()` itself fails) never gets a chance to be
    acked -- nobody ever receives it. `z_port_send()` frees that one
    immediately, right at the failed call site, rather than pushing it
    onto the pending queue at all -- pushing it would leave a
    permanent gap with no ack ever coming to pop it, misaligning every
    later entry the same way skipping an ack would. This is the DATA
    side of delivery failure; **the ack side (below) is handled
    differently, on purpose**, since giving up isn't a safe option
    there.
  - **`z_port_send_ack()` retries instead of giving up, unlike every
    other send in this file.** A lost DATA message is self-contained
    -- the bullet above shows it's caught and cleanly freed, nothing
    else is affected. A lost ack is not self-contained: it corrupts
    the FIFO position mapping for every send after it on that
    connection, permanently, since the whole scheme depends on a
    strict 1:1 correspondence between sends and acks. So a short,
    bounded retry (`Z_PORT_ACK_RETRY_TICKS`, ~0.5s) is the right
    tradeoff specifically here, even though nothing else in `zport.h`
    retries: the failure this guards against (a receiving mailbox
    transiently full) resolves within the receiver's own next
    scheduling slice or two under this system's preemptive scheduling,
    so the retry usually costs nothing observable, while the
    alternative (silent, permanent desync) is worse than any bounded
    wait. If the retry genuinely exhausts (something worse than
    transient congestion), it logs and gives up -- `z_port_send()`'s
    own backpressure is still the correct backstop from there, same as
    for a peer that's stopped acking entirely.
  - **Deliberately scoped to `zport.h`'s DATA channel, not a universal
    fix.** One-shot RPC-style replies elsewhere (DHCP, DNS, TFTP,
    `pong`) are left as-is, per this bullet's opening paragraph --
    already bounded, already negligible, not worth the extra
    bookkeeping. This mechanism could generalize (a shared
    `zmsg.h`-level ack primitive any protocol could opt into, or a
    refcount-in-the-object scheme if this system ever grows a
    broadcast primitive with more than one recipient per payload) if
    a second real need for it ever shows up -- not built speculatively
    ahead of that.
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
  previous chunk was received. `zport.h`'s own `Z_PORT_DATA_ACK`
  (above) solves the same underlying problem a different way -- an
  explicit ack message rather than a pull request doing double duty --
  since `zport`'s DATA channel is push-based in both directions, not
  pull-based like `zstream`; worth remembering both patterns exist
  before reaching for a third if a similar need comes up again.


## Payload lifetime, and the leak it used to cause

A str/list/map payload is **borrowed**: `k_msg_send()` stores the
sender's own pointer, and `k_msg_read()` resolves it out of the
sender's address space (`z_translate()`, `sw/os/msg.c`) only when the
recipient actually reads the message. The sender therefore cannot free
it at the point of sending — the recipient may not have looked yet.

For a long time the answer to that was simply not to free at all, and
the call sites said so in as many words ("intentionally never freed").
That is survivable for a message sent once at startup. It is not
survivable for one sent on a repeating user action, and one of these
was:

`wm`'s `send_win_rect()` built a five-key `Z_MAP` with `z_obj_map()`
and `z_map_set()` for every `Z_WM_WINDOW_MOVED` — one per completed
drag, per Alt+Arrow step, and per resize. Measured at **384 bytes a
time**, against the 8KB `wm` gets for stack and heap together
(`Z_PROC_STACK_SIZE_SMALL`, `sw/os/kernel.h`). About twenty window
moves exhausted the heap. `z_obj_map()` then wrote through the NULL
that `malloc()` returned, and the machine went down — with nothing in
the symptom pointing at either the leak or the allocator.

Two changes, and both matter:

- **Frequent payloads are built in static storage.** `send_win_rect()`
  (`wm.c`), `z_win_create_cb()` and `z_win_set_title()` (`zwin.c`) now
  fill pre-declared `z_obj_t`/`z_obj_table_t` arrays and point string
  values at literals or fixed buffers. `z_translate()` reaches `.bss`
  and `.rodata` exactly as well as it reached the heap; it just cannot
  run out. Where the send is fire-and-forget, a small RING of slots is
  used so a payload isn't overwritten before the recipient reads it —
  which narrows the borrow window rather than closing it, but replaces
  a certainty with something that does not happen in practice.
- **`z_obj_map()`/`z_obj_list()` check their allocations.** They now
  return `Z_NONE` rather than dereferencing NULL, so an exhausted heap
  anywhere degrades to a message with a missing payload instead of
  taking the system down. Every reader in this codebase already checks
  the type of what it got.

The rule to take from this: **if a message is sent in response to
something the user can do repeatedly, its payload must not be
allocated.** Use a packed scalar (`Z_WM_REDRAW`, `Z_WM_MOUSE` and
`Z_WM_KEY` all do) or static storage. The leak is invisible until the
process dies, and the crash lands nowhere near the cause.
