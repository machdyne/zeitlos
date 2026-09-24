#ifndef ZTTS_H
#define ZTTS_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Text-to-speech: the wire protocol between apps and sw/apps/tts.
 *
 * Apps should not use this header directly -- use zspeak.h, which
 * caches the service lookup, copies text into storage whose lifetime
 * is safe for a fire-and-forget message, and costs about ten
 * instructions when speech is off. This header is the contract that
 * library and the service agree on. See docs/tts.md.
 *
 * -- The service --
 *
 * `tts` registers the base name "tts" and therefore answers to
 * "tts0". A second instance notices the first and exits, so there is
 * never a "tts1" to find. When speech is turned off the service EXITS
 * rather than muting: every z_speak() in the system then takes the
 * cheap "nobody there" path, and the memory comes back.
 *
 * -- Subjects --
 *
 * All in 0x5454xxxx ("TT"), far from every other subject in the tree
 * (the window manager's are 100..121, zport's and zstream's are small
 * integers too). That matters because a cached pid can briefly outlive
 * the process it named (docs/tts.md, "Cost"): a message that lands on
 * an unrelated process must be one that process has no case for.
 */

#include <stdint.h>

#define Z_TTS_SERVICE_BASE		"tts"
#define Z_TTS_SERVICE			"tts0"

// -- app -> tts --

// Speak text. obj is a Z_STR. tag is Z_TTS_TAG(flags, mark).
//
// The service copies the text into its own queue the moment it reads
// the message, so the sender's buffer only has to survive until then
// -- which zspeak.c's ring guarantees in practice (docs/tts.md).
#define Z_TTS_SAY				0x54540001u

// Stop speaking and discard everything queued. No payload. Every
// discarded utterance that carried a mark is reported to its sender
// as Z_TTS_MARK_CANCELLED.
#define Z_TTS_STOP				0x54540002u

// Speak the last utterance again. No payload.
#define Z_TTS_REPEAT			0x54540003u

// Say "Speech off", finish saying it, and exit. No payload. This is
// how Super+S turns speech off (sw/apps/wm).
#define Z_TTS_QUIT				0x54540004u

// Change a voice setting. obj is a Z_UINT32 from Z_TTS_SET_PACK().
#define Z_TTS_SET				0x54540005u

// Become the narrator, or stop being it. obj is a Z_UINT32: a lease in
// seconds, or 0 to give it up. While a narrator holds the lease, SAY,
// STOP and REPEAT from every OTHER process are ignored -- a marked SAY
// is answered Z_TTS_MARK_CANCELLED at once -- so a scripted demo
// (sw/apps/automate) is not talked over, or cut off, by wm announcing
// each window that opens. Every SAY from the narrator renews the
// lease; so does sending this again. A narrator that dies simply lets
// it run out. Z_TTS_QUIT (Super+S) still works: the user can always
// turn speech off. See docs/tts.md, "Narration".
#define Z_TTS_NARRATE			0x54540006u

// -- tts -> app --
//
// Sent only for utterances that carried a non-zero mark, and only to
// the process that sent them. obj is a Z_UINT32 holding the mark, and
// the tag carries it too so z_msg_wait() can match on it.
//
// Exactly one of the two arrives for every marked utterance the
// service accepted -- including one it refused outright (queue full,
// or Z_TTS_F_LOW while busy), which is reported CANCELLED at once.
// A sender that loses one (its own mailbox full) must not wait
// forever; zsayall-style readers use a timeout.
#define Z_TTS_MARK_DONE			0x54540010u
#define Z_TTS_MARK_CANCELLED	0x54540011u

// -- flags (low 8 bits of the tag) --

// Discard whatever is being said and everything queued, then say
// this. What a focus change wants: holding an arrow key speaks only
// where you land, not every item you passed.
#define Z_TTS_F_INTERRUPT		0x01u

// Read the text character by character ("c, a, t").
#define Z_TTS_F_SPELL			0x02u

// Drop this if anything is being said or queued. For chatter that is
// only worth hearing when nothing more important is.
#define Z_TTS_F_LOW				0x04u

// This utterance runs on into the next one -- a line that wrapped in
// the middle of a sentence. The voice leaves out the pause and the
// falling pitch that end a sentence, so a paragraph read a line at a
// time (sw/common/zsayall.h) still sounds like one paragraph.
#define Z_TTS_F_CONTINUES		0x08u

// Without Z_TTS_F_INTERRUPT an utterance is queued behind whatever is
// already there -- that is the default, and has no flag.

#define Z_TTS_TAG(flags, mark)	(((uint32_t)(mark) << 16) | ((uint32_t)(flags) & 0xffu))
#define Z_TTS_TAG_FLAGS(tag)	((tag) & 0xffu)
#define Z_TTS_TAG_MARK(tag)		(((tag) >> 16) & 0xffffu)

// -- settings --
//
// One parameter per message, 8-bit id and 24-bit value. Values out of
// range are clamped by the service, not refused.

#define Z_TTS_PARAM_RATE		1	// words per minute, 80..450
#define Z_TTS_PARAM_PITCH		2	// base pitch in Hz, 50..300
#define Z_TTS_PARAM_VOLUME		3	// 0..255
#define Z_TTS_PARAM_VOICE		4	// Z_TTS_VOICE_*, for the rest of the session

// Voices, for Z_TTS_PARAM_VOICE. RECORDED needs a speech pack with
// diphones; without one the formant voice speaks, as it always would.
// NEXT switches between the recorded voice and the formant one (wm's
// Super+E), and the service says which it now is, in that voice.
#define Z_TTS_VOICE_RECORDED		0
#define Z_TTS_VOICE_MALE		1
#define Z_TTS_VOICE_FEMALE		2
#define Z_TTS_VOICE_NEXT		0xff
// OR'd into a voice: switch without saying so. For a script that wants
// the change heard in its own next sentence (sw/apps/automate).
#define Z_TTS_VOICE_QUIET		0x100

#define Z_TTS_SET_PACK(param, value) \
	(((uint32_t)(param) << 24) | ((uint32_t)(value) & 0xffffffu))
#define Z_TTS_SET_PARAM(v)		(((v) >> 24) & 0xffu)
#define Z_TTS_SET_VALUE(v)		((v) & 0xffffffu)

// Longest single utterance the service will hold. Longer text is
// truncated, not refused. Sized for the whole clipboard
// (Z_WM_CLIP_MAX, zwm.h), which is the longest thing anything sends.
#define Z_TTS_UTTER_MAX			4096

#endif
