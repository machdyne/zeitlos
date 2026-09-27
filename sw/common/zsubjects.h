#ifndef ZSUBJECTS_H
#define ZSUBJECTS_H

/*
 * Zeitlos -- message subject numbers: who owns which.
 *
 * A message's `subject` says what kind it is (docs/messaging.md). Every
 * protocol's subjects must be distinct from every other's, because one
 * process usually speaks several: an app hears from wm, net and term at
 * once, and a subject two protocols share is read as whichever of the
 * two the receiver happens to check first. That happened -- wm's
 * subjects grew past 119 into the port protocol's 120-125 and the REPL
 * protocol's 130-132, and ask's 313-322 landed on net's 313-317 -- and
 * the compiler said nothing, because nothing told it which numbers
 * belonged to whom.
 *
 * This file does. Each protocol owns ONE BLOCK: a base and a size,
 * below. Its header defines every subject as base + offset and includes
 * this file, so a subject's number is written down in exactly one
 * place, and it checks with Z_SUBJECTS_IN() that its last subject is
 * still inside its block. The chain of checks at the bottom makes the
 * blocks themselves disjoint. A collision is now a compile error.
 *
 * -- adding a protocol --
 *
 *   1. Give it a block here, IN ORDER OF BASE, with room to grow, and
 *      add the check that links it to the block before it.
 *   2. In its header: #include "zsubjects.h", define each subject as
 *      (Z_SUBJ_<NAME> + n), and check the last one:
 *        Z_SUBJECTS_IN(Z_SUBJ_<NAME>, Z_<NAME>_<LAST>);
 *
 * A protocol private to one app -- messages between its own processes,
 * say -- still takes a block: whoever else it talks to hears them too.
 *
 * -- the blocks --
 *
 * Values that had been in use are kept wherever they did not collide
 * (wm, net, stream, speech and wm's game mode), so only the protocols
 * that overlapped moved: ports, the REPL protocol and term's up out of
 * wm's hundred, web's fetch service and ask out of net's.
 */

//                        base          size   header      protocol
#define Z_SUBJ_WM          100          // 100 zwm.h       wm <-> apps
#define Z_SUBJ_WM_N        100
#define Z_SUBJ_PORT        200          //  16 zport.h     connections (ports.md)
#define Z_SUBJ_PORT_N      16
#define Z_SUBJ_REPL        216          //  16 zrepl.h     term <-> repl/posix
#define Z_SUBJ_REPL_N      16
#define Z_SUBJ_TERM        232          //  16 zterm.h     apps -> term
#define Z_SUBJ_TERM_N      16
#define Z_SUBJ_NET         300          //  40 znet.h,     net's services
#define Z_SUBJ_NET_N       40           //     zntp.h
#define Z_SUBJ_WEB         340          //   8 zweb.h      web's fetch service
#define Z_SUBJ_WEB_N       8
#define Z_SUBJ_ASK         348          //  32 zask.h      ask
#define Z_SUBJ_ASK_N       32
#define Z_SUBJ_STREAM      400          //  16 zstream.h   byte streams
#define Z_SUBJ_STREAM_N    16
#define Z_SUBJ_TTS         0x54540000u  // 64K ztts.h      speech ("TT", tts.md)
#define Z_SUBJ_TTS_N       0x10000
#define Z_SUBJ_WM_GAME     (-32768)     //  16 zwm.h       wm game mode
#define Z_SUBJ_WM_GAME_N   16

// Is subject `s` inside the block based at `base`? As a compile-time
// check; `base##_N` is the block's size.
#define Z_SUBJECTS_IN(base, s) \
	_Static_assert((unsigned long)((s) - (base)) < (unsigned long)(base##_N) && \
		(s) >= (base), #s " is outside its block " #base " (zsubjects.h)")

// The blocks, in order of base, each clear of the one before. (The
// game block's negative base is 0xffff8000 as a subject, which a
// uint32_t is: last.)
#define Z_SUBJ_AFTER_(a, b) \
	_Static_assert((unsigned long)(b) >= (unsigned long)(a) + (unsigned long)(a##_N), \
		#b " overlaps " #a " (zsubjects.h)")
Z_SUBJ_AFTER_(Z_SUBJ_WM, Z_SUBJ_PORT);
Z_SUBJ_AFTER_(Z_SUBJ_PORT, Z_SUBJ_REPL);
Z_SUBJ_AFTER_(Z_SUBJ_REPL, Z_SUBJ_TERM);
Z_SUBJ_AFTER_(Z_SUBJ_TERM, Z_SUBJ_NET);
Z_SUBJ_AFTER_(Z_SUBJ_NET, Z_SUBJ_WEB);
Z_SUBJ_AFTER_(Z_SUBJ_WEB, Z_SUBJ_ASK);
Z_SUBJ_AFTER_(Z_SUBJ_ASK, Z_SUBJ_STREAM);
Z_SUBJ_AFTER_(Z_SUBJ_STREAM, Z_SUBJ_TTS);
_Static_assert((unsigned long)(unsigned int)Z_SUBJ_WM_GAME >=
	(unsigned long)Z_SUBJ_TTS + Z_SUBJ_TTS_N, "Z_SUBJ_WM_GAME overlaps Z_SUBJ_TTS");

#endif
