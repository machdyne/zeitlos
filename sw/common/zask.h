#ifndef ZASK_H
#define ZASK_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The ask service: how any process asks `ask` (sw/apps/ask) a
 * question about the installed information distribution. See
 * docs/ask_app.md.
 *
 * -- what this service does and does not do --
 *
 * It FINDS TEXT. It does not write text. Every byte a caller gets back
 * through Z_ASK_TEXT is a byte-for-byte range of a document on the
 * card, and Z_ASK_RESULT hands out the path and offset of that range
 * so the caller can verify it, open it in `read`, or ignore the
 * preview entirely and go to the source.
 *
 * That is a claim about PROVENANCE, not about authorship. Part of the
 * corpus is itself LLM-generated -- the Ark Scroll and the Ark Codex
 * both are -- so "written by a person" would be false. What holds is
 * that `ask` never ADDS anything, and that a caller can always tell
 * where a passage came from. Datasets that are model-written are
 * flagged in the pack index and should be surfaced to the user; see
 * docs/ask_app.md, "Which is why model-written datasets are marked".
 *
 * There is no generative endpoint and there is not going to be one. A
 * model small enough to run on this machine is exactly the size that
 * produces fluent, plausible, wrong text, and a machine meant to be
 * useful when nothing else is available must not be able to invent an
 * answer about tourniquets. See docs/ask_app.md, "The rule".
 *
 * It also does not index the user's files. `ask` answers from the
 * datasets in the installed distribution and nothing else -- this is
 * not a filesystem search service, and a caller that wants one wants
 * something else.
 *
 * -- why this is a service and not a library --
 *
 * Same reasoning as sw/common/zweb.h, with one addition that is
 * specific to this app: the resident index is 1.5MB at the smallest
 * distribution tier and several megabytes at the largest. Linking that
 * into every caller is not an option -- there is one copy and one
 * owner. Loading it also costs seconds off the card (docs/sdcard.md
 * measures 604 KB/s through FatFs), which a service pays once at
 * startup rather than per caller.
 *
 * -- finding the service --
 *
 *     uint32_t ask_pid;
 *     if (!z_pid_lookup(Z_ASK_SERVICE_NAME, &ask_pid)) {
 *         // ask is not running; z_proc_run("ask") and retry, or
 *         // tell the user. Do NOT assume a pid.
 *     }
 *
 * There is deliberately no Z_PID_ASK constant, for the reason zweb.h
 * gives: Z_PID_NET exists only because it predates the registry
 * (sw/os/pidreg.h), and there is no reason to add a second one.
 *
 * -- the shape of a query --
 *
 *   1. Z_ASK_QUERY   ->  Z_ASK_RESULT
 *      The question goes in, a ranked list of hits comes back. Each
 *      hit carries enough to display a result line and to open the
 *      document, but NOT the passage text.
 *
 *   2. Z_ASK_PREVIEW ->  Z_ASK_TEXT       (optional, per hit)
 *      The bytes of one hit. Separate because a caller that only
 *      wants to know WHERE the answer is -- a launcher, a shell
 *      command, `read` resolving a cross-reference -- should not pay
 *      for a card read it will throw away.
 *
 * A query takes time. See "Responsiveness" below; it is the part of
 * this protocol most likely to be got wrong by a client.
 */

#include <stdint.h>

// The name to pass to z_pid_lookup(). `ask` registers the basename
// "ask" (z_pid_register(), sw/os/pidreg.h), so the first instance is
// "ask0" -- same convention as "web0", "net0", "wm0".
#define Z_ASK_SERVICE_NAME   "ask0"

// -- message subjects --
//
// 313-322, continuing the ONE shared sequence that znet.h (302-305,
// 308-309), zntp.h (306-307) and zweb.h (310-312) draw from. Nothing
// enforces this; the numbers are global by convention only, and
// reusing one does not fail to compile and does not look wrong at
// runtime -- a dispatch chain of `else if` just delivers the message
// to the first handler that matches. See net.c's SUBJECT COLLISION
// CHECK, which exists because exactly that happened once.
//
// The next subject added anywhere starts at 323.

// requester -> ask:
//   Z_MAP {
//     "q":       Z_STR     required, the question, as typed
//     "n":       Z_UINT32  optional, max hits to return.
//                          Default Z_ASK_HITS_DEFAULT, capped at
//                          Z_ASK_HITS_MAX.
//     "pack":    Z_STR     optional, restrict to one installed pack
//                          ("minimal", "medium", ...). Empty or
//                          absent queries every pack on the card.
//     "dataset": Z_STR     optional, restrict to one dataset name
//                          ("codex", "medline", "docs", ...). Empty
//                          or absent means all of them.
//     "mode":    Z_UINT32  optional, Z_ASK_MODE_*. Default HYBRID.
//   }
//
// One query at a time, per the same "one X at a time" simplification
// every other service app in this tree makes. A second query while one
// is in flight CANCELS the first rather than queueing behind it --
// which is the opposite of what `web` does with fetches, deliberately:
// a fetch has a side effect worth finishing and a query does not, and
// the overwhelmingly common case here is a user editing what they
// typed. Queueing would make the app feel slower the more the user
// corrected themselves.
//
// The cancelled requester gets a Z_ASK_RESULT with "cancelled" set
// rather than silence, so a client is never left waiting on a reply
// that is not coming.
#define Z_ASK_QUERY            313

// ask -> requester, reply to Z_ASK_QUERY (same tag):
//   Z_MAP {
//     "ok":        Z_UINT32   0 or 1
//     "cancelled": Z_UINT32   1 if superseded or Z_ASK_CANCEL'd
//     "count":     Z_UINT32   hits in "hits"
//     "scanned":   Z_UINT32   chunks examined, for diagnostics
//     "ms":        Z_UINT32   wall time spent
//     "hits":      Z_LIST of Z_MAP {
//         "id":      Z_UINT32   chunk id, for Z_ASK_PREVIEW
//         "score":   Z_UINT32   0..1000, see below
//         "path":    Z_STR      card path, e.g. "/ark/books/00000009.txt"
//         "off":     Z_UINT32   byte offset of the passage
//         "len":     Z_UINT32   byte length of the passage
//         "title":   Z_STR      document title, e.g. "FM 21-76 SURVIVAL"
//         "head":    Z_STR      heading path, "" if the document has none
//         "dataset": Z_STR      which dataset it came from
//         "pack":    Z_STR      which installed pack it came from
//     }
//     "error":     Z_STR       only when ok == 0
//   }
//
// "score" IS 0..1000 AND IS NOT A PROBABILITY. It is a monotone
// rescaling of the fused rank score, and its only defined property is
// that a higher one ranked above a lower one within the same reply.
// Comparing scores across queries is meaningless.
//
// It is in the protocol anyway, for one reason: a client has to be
// able to SHOW IT. The dangerous failure of this whole system is not
// "no result", it is a confident wrong result that looks exactly like
// a right one. A user who can see that the best hit scored 410 and
// that the app said so is being told the truth. Clients should display
// the score and should say something when every hit is below
// Z_ASK_SCORE_WEAK.
//
// "ok" is about the QUERY, not about whether the answer is any good. A
// query that ran correctly and found nothing relevant is ok == 1 with
// count == 0, and that is a legitimate and common answer -- "no
// passage in this corpus answers that" is the right response to most
// of what anyone will type. ok == 0 means the service could not run
// the query at all: no distribution installed, index failed to load.
//
// "path" is a full card path and always fits in Z_WM_ARG_MAX (96
// bytes, zwm.h) with room for a "#<offset>" suffix. That is not an
// accident -- see docs/ask_app.md on the 8.3 numbering -- and it is
// what lets a client hand a hit straight to `read`:
//
//     char arg[Z_WM_ARG_MAX];
//     snprintf(arg, sizeof(arg), "%s#%u", path, off);
//     // Z_WM_SET_ARG to wm, then z_proc_run("read")
#define Z_ASK_RESULT           314

// requester -> ask: Z_MAP { "id": Z_UINT32, "max": Z_UINT32 }
//
// "id" is a chunk id from a Z_ASK_RESULT hit. "max" optionally caps
// the bytes returned; 0 means the whole passage.
//
// Valid until the next Z_ASK_QUERY from the same requester. Ids are
// positions in the index, not handles, so a stale one is not
// dangerous -- it just returns a different passage than the caller
// expected, which is why the reply echoes the path and offset back.
#define Z_ASK_PREVIEW          315

// ask -> requester, reply to Z_ASK_PREVIEW (same tag):
//   Z_MAP {
//     "ok":    Z_UINT32
//     "id":    Z_UINT32   echoed
//     "path":  Z_STR      echoed, so a caller can verify what it got
//     "off":   Z_UINT32   echoed
//     "text":  Z_BLOB     the passage bytes, UTF-8, NOT NUL-terminated
//     "error": Z_STR      only when ok == 0
//   }
//
// The blob is BORROWED from `ask`'s memory and is only valid until the
// caller sends its own next message -- the ordinary zmsg.h rule (see
// that header's note on Z_BLOB). Copy it if you need to keep it.
//
// This is a byte range of a file on the card, verbatim. It has not
// been reflowed, summarised or cleaned up, and it may begin or end
// mid-paragraph at a chunk boundary. A client that wants to trim it to
// whole sentences should do so itself and should not silently join
// ranges from two different hits -- see docs/ask_app.md, "The rule".
#define Z_ASK_TEXT             316

// requester -> ask: Z_UINT32, non-zero.
//
// Abandons the query in flight. No reply to this message; the
// cancelled query's own Z_ASK_RESULT arrives with "cancelled" set.
// Harmless if nothing is running.
//
// Cancellation is immediate: the scan holds no lock and allocates
// nothing per slice, so this costs the price of one flag test at the
// next slice boundary. See docs/ask_app.md, "Responsiveness".
#define Z_ASK_CANCEL           317

// requester -> ask:
//   Z_MAP {
//     "dataset": Z_STR     optional; absent lists the datasets
//                          themselves, present lists that dataset's
//                          documents
//     "from":    Z_UINT32  optional, first index (paging)
//     "n":       Z_UINT32  optional, how many
//   }
//
// The browse tree, which exists because the card holds numbered files
// (8.3, FF_USE_LFN 0) and `files` therefore shows a wall of digits.
// `ask` has the index, so `ask` is where Ark is browsable by title.
#define Z_ASK_BROWSE           318

// ask -> requester, reply to Z_ASK_BROWSE (same tag):
//   Z_MAP {
//     "ok":     Z_UINT32
//     "total":  Z_UINT32   entries available, for paging
//     "items":  Z_LIST of Z_MAP {
//         "name":  Z_STR      dataset name, or document title
//         "path":  Z_STR      "" for a dataset, card path for a document
//         "n":     Z_UINT32   document count, for a dataset
//     }
//   }
#define Z_ASK_LIST             319

// requester -> ask: no payload.
#define Z_ASK_INFO             320

// ask -> requester, reply to Z_ASK_INFO (same tag):
//   Z_MAP {
//     "ok":         Z_UINT32  0 if no distribution is installed
//     "name":       Z_STR     distribution name, e.g. "minimal"
//     "version":    Z_STR     recipe version
//     "built":      Z_STR     build date, YYYY-MM-DD
//     "dsid":       Z_UINT32  dataset id -- see below
//     "encoder":    Z_STR     "bow", "torch", ... of the FIRST pack
//     "packs":      Z_LIST of Z_MAP {
//         "name":       Z_STR     "minimal", "medium", ...
//         "version":    Z_STR
//         "dsid":       Z_UINT32
//         "encoder_id": Z_UINT32  hash of the WEIGHTS, not the name.
//                                 Packs sharing an id share a vector
//                                 space and one loaded copy.
//         "ndocs":      Z_UINT32
//         "nchunks":    Z_UINT32
//     }
//     "ndocs":      Z_UINT32
//     "nchunks":    Z_UINT32
//     "datasets":   Z_UINT32
//     "resident":   Z_UINT32  bytes of RAM the index is holding
//     "accel":      Z_UINT32  1 if the scan is running in hardware
//   }
//
// "dsid" is the low 32 bits of a hash over the recipe and every
// document's bytes. Every index file on the card carries it, and `ask`
// refuses to load a set whose ids disagree: a coarse.zcv from one
// distribution against a chunks.zct from another produces confidently
// ranked results pointing at the wrong paragraphs, which is the worst
// failure this system can have and is otherwise completely silent.
//
// "resident" is the total across every loaded pack. Loading is
// reported through Z_ASK_PROGRESS rather than blocking: at a realistic
// 300-600 KB/s, a 1.5MB pack is three to five seconds, and an app that
// shows nothing for five seconds at launch is an app that looks
// broken. docs/sdcard.md's 604 KB/s is the optimistic end.
//
// "accel" reports what is actually happening, not what the bitstream
// has. `ask` self-tests the accelerator against a known vector before
// trusting it and falls back to software on any mismatch -- the
// arrangement sw/apps/web/ecdsa.c uses for rtl/montmul.v, for the same
// reason: a half-wired accelerator otherwise looks like a bad model
// rather than bad hardware.
#define Z_ASK_INFO_REPLY       321

// ask -> requester, unsolicited, during a long operation:
//   Z_MAP {
//     "what":  Z_UINT32   Z_ASK_PROG_*
//     "done":  Z_UINT32   units completed
//     "total": Z_UINT32   units expected, 0 if genuinely unknown
//     "label": Z_STR      short, for a status line ("loading medium")
//   }
//
// Sent to whoever is waiting on the operation in flight, at most once
// per Z_ASK_SCAN_SLICE or per file read. A client is free to ignore
// these -- they arrive between the request and its reply and carry a
// different subject, so a client that only matches on the reply tag
// is unaffected.
//
// This exists because the two slow things here are slow for reasons
// that cannot be engineered away on this machine: loading a pack is
// bounded by card throughput, and scanning is bounded by SDRAM. A
// progress bar is the honest interface to a known-duration wait, and
// "total" is a real count rather than a guess.
#define Z_ASK_PROGRESS         322

#define Z_ASK_PROG_LOADING     0    // reading a pack off the card
#define Z_ASK_PROG_SCANNING    1    // coarse scan, units are vectors
#define Z_ASK_PROG_RERANK      2    // fine vectors, units are candidates

// -- limits --

// Hits returned by default. Small because a result list is read by a
// human on a 640x480 screen, not consumed by a program.
#define Z_ASK_HITS_DEFAULT     8

// Hard cap. Each hit carries two strings, and z_msg_read()'s scratch
// budget (Z_MSG_MAX_TABLES / Z_MSG_MAX_ITEMS, zmsg.h) is what
// ultimately bounds this -- a reply that exceeds it does not fail
// cleanly, it resolves wrong.
#define Z_ASK_HITS_MAX         16

// Below this score, a client should tell the user the answer is weak
// rather than presenting it as an answer. Calibrated so that the
// "no passage in this corpus answers that" case lands below it.
#define Z_ASK_SCORE_WEAK       450

// Query modes. HYBRID is the default and the one to use.
//
// LEXICAL and DENSE exist so the two halves can be compared ON DEVICE
// against the same index the host measured -- which is how a packing
// bug gets caught. They are not a user-facing choice and `ask`'s own
// UI does not offer them.
#define Z_ASK_MODE_HYBRID      0
#define Z_ASK_MODE_LEXICAL     1
#define Z_ASK_MODE_DENSE       2

// Vectors scanned between returns to the message loop.
//
// This is the responsiveness dial and it is a real one: the coarse
// scan is the only unbounded loop in the app, and a process that spins
// is a process that makes the whole desktop feel dead (see
// docs/app_runtime.md, "Every app now yields").
//
// 2048 vectors at 32 dimensions is ~65K MACs, a few milliseconds in
// software on a 48MHz core -- well inside one frame, so the window
// redraws and ESC is answered while a query runs. Raise it and
// cancellation gets laggy; lower it and the per-slice overhead starts
// to show.
#define Z_ASK_SCAN_SLICE       2048

// Longest question accepted, bytes including the NUL. Longer is
// truncated rather than refused: a question is a handful of words and
// anything past this is a paste.
#define Z_ASK_QUERY_MAX        160

#endif
