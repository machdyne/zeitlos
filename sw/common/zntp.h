#ifndef ZNTP_H
#define ZNTP_H

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Time-sync protocol between the net app (sw/apps/net/ntp.c) and any
 * process that wants to prod it -- currently sw/apps/clock's Sync
 * button, which is the reason this exists.
 *
 * Nothing needs this just to READ the time: the RTC is memory-mapped
 * hardware and sw/common/zrtc.h reads it with a load instruction, no
 * messaging involved. This is only for asking the network for a fresh
 * one, which needs the process that owns the network.
 *
 * -- why this is not in znet.h --
 *
 * It very nearly is, and the subject numbers below CONTINUE that
 * file's sequence rather than starting their own (znet.h ends at 305).
 * Keeping them in one space is not optional -- two subjects with the
 * same number would be delivered to the same handler and the bug would
 * look like the wrong feature firing.
 *
 * They live in a separate header only so that adding time sync did not
 * require editing znet.h, whose contents are stable and widely
 * included. If these ever grow past a couple of subjects, folding them
 * into znet.h and deleting this file is the right move.
 *
 * KEEP THE NUMBERING IN SYNC: the next subject added to znet.h must
 * start at 308, not 306.
 *
 * That warning has already been ignored once, with exactly the
 * consequence described above: Z_NET_SSH_PREPARE was added at 306 by
 * reading znet.h alone, so every `ssh` command was delivered to
 * handle_ntp_sync() -- net's dispatch chain matched Z_NET_NTP_SYNC
 * first -- and repl waited forever for a reply nothing would send.
 * Nothing looked wrong in any log, because net HAD handled the
 * message.
 *
 * sw/apps/net/net.c now carries a compile-time check (search for
 * SUBJECT COLLISION CHECK) comparing every subject it dispatches
 * against every other. A new subject should be added to that list at
 * the same time it is defined here or in znet.h -- a comment is a
 * request, and this one shows what happens when it is not read.
 */

// requester -> net: Z_NONE. Asks net to sync the RTC from its NTP
// server now, rather than waiting for the next scheduled sync (hourly
// -- see ntp.c's NTP_SYNC_INTERVAL_TICKS).
//
// Fire-and-forget: there is no reply to this and nothing to wait for.
// The sync is a network round trip that may take seconds or fail
// entirely, and a caller that blocked on it would be blocking a UI on
// a public server's response time. A caller that wants to know whether
// it worked watches the RTC's own seconds jump, or asks with
// Z_NET_NTP_STATUS below.
//
// Ignored if a sync is already in flight, so an impatient double-click
// costs nothing.
#define Z_NET_NTP_SYNC          306

// requester -> net: Z_NONE. Asks how the clock is doing.
//
// Reply (same tag) is Z_MAP with:
//   "enabled"  Z_UINT32  0/1 -- whether ntp is running at all. 0 means
//                        built with NTP_ENABLE=0, or this bitstream
//                        has no RTC to set (see zrtc.h).
//   "synced"   Z_UINT32  0/1 -- whether a sync has ever succeeded
//                        since net started.
//   "age"      Z_UINT32  ticks since the last successful sync
//                        (z_uptime_ticks() units, ~732Hz), or 0 if
//                        never. Ticks, not wall-clock seconds,
//                        deliberately: this answers "how stale is the
//                        clock", and the wall clock is the thing in
//                        question.
#define Z_NET_NTP_STATUS        307

#endif
