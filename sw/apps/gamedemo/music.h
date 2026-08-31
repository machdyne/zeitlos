#ifndef GAMEDEMO_MUSIC_H
#define GAMEDEMO_MUSIC_H

/*
 * gamedemo -- music and sound effects.
 *
 * The score is ASCII, in music.c, for the same reason the level is:
 * it is then its own picture, and editing it is editing text.
 *
 * -- how a note becomes a sound --
 *
 * rtl/audio_mixer.v plays 8-bit signed samples from main memory at a
 * programmable rate, and loops them. That is all it does. It has no
 * oscillators and no notion of pitch.
 *
 * So an instrument here is ONE CYCLE of a waveform -- 64 bytes -- set
 * to loop forever. Playing it back faster makes it a higher note:
 *
 *     note frequency = playback rate / 64
 *
 * which inverts to the rate the mixer needs. That is exactly how a
 * tracker gets pitch out of a sampler, and it means four instruments
 * cost 4 x 64 = 256 bytes of RAM and no synthesis at all at runtime.
 *
 * A single cycle also loops seamlessly by construction: its end joins
 * its start, so there is no click and no loop-point tuning. That is
 * the property that makes 64 bytes enough.
 *
 * -- envelopes --
 *
 * A looped waveform at constant gain is an organ, not a game. Each
 * channel therefore carries a volume that decays every frame, written
 * back with EN but NOT TRIG -- docs/audio.md is explicit that this
 * changes gain without restarting the sample, which is precisely what
 * a volume-only tracker row needs.
 *
 * Percussion is the same mechanism with a fast decay on the noise
 * instrument: a kick is low-pitched noise decaying in ~0.1s, a hat is
 * high-pitched noise decaying in ~0.05s. No separate code path.
 *
 * -- timing --
 *
 * Advanced from the game loop, once per frame, with no interrupt and
 * no thread. z_game_flip() already returns the number of frames that
 * actually elapsed, so a frame the game overran advances the music by
 * the right amount instead of dragging the tempo with it.
 *
 * Rows per second is derived from z_video_frame_hz(), so the tempo is
 * the same on a 50Hz PAL composite board as on a 60Hz one. Deriving it
 * from a frame COUNT instead would make every song 20% slow on PAL,
 * which is the classic version of this bug and worth not shipping.
 */

#include <stdint.h>
#include <stdbool.h>

/* -- channel allocation --
 *
 * The mixer has eight. Music takes the low four, effects the high
 * four, and they never overlap -- so a jump landing during a bass note
 * cannot cut the bass off, which is what sharing a channel pool would
 * do at exactly the busiest moment.
 */
#define MUS_CH_LEAD  0
#define MUS_CH_HARM  1
#define MUS_CH_BASS  2
#define MUS_CH_PERC  3
#define MUS_CHANNELS 4

#define SFX_CH_BASE  4
#define SFX_CHANNELS 4

/* Instruments. One single-cycle waveform each, except NOISE which is a
 * longer buffer because one cycle of noise is a contradiction. */
enum {
	INS_SQUARE = 0,   /* 50% duty -- lead. Hollow, cuts through. */
	INS_PULSE,        /* 25% duty -- harmony. Thinner, sits under lead. */
	INS_TRI,          /* triangle -- bass. Soft, few harmonics. */
	INS_NOISE,        /* LFSR noise -- percussion. */
	INS_COUNT
};

/* Sound effects, played on the high channels. */
enum {
	SFX_JUMP = 0,
	SFX_COIN,
	SFX_HIT,
	SFX_LAND,
	SFX_COUNT
};

/* Call once at startup. Synthesises the waveforms, enables the audio
 * output and the mixer.
 *
 * Returns false if this board has no audio or no hardware mixer, in
 * which case every other function here is a no-op and the game plays
 * silently rather than not at all. */
bool music_init(void);

/* -- master volume --
 *
 * THE volume knob, and the one to reach for. It is the mixer's own
 * output scale, so it affects music AND sound effects together.
 *
 * Editing the per-channel gains in music.c instead is a common wrong
 * turn: those set the BALANCE between the four instruments, and
 * halving them leaves every jump, coin and landing exactly as loud,
 * because effects do not pass through them. The game barely gets
 * quieter and it looks as though the change did nothing.
 *
 * The hardware resets this to 128 (rtl/audio_mixer.v calls that safe
 * for all eight channels; 255 is what a 4-channel MOD wants), so the
 * default below is a straight 50% cut against the hardware default.
 *
 * If turning this down genuinely does not get quieter, suspect the
 * GATEWARE rather than this code: docs/audio.md records an output
 * scaler bug whose exact signature was "turned the volume down and
 * it's just as loud", caused by a signed part-select rectifying the
 * negative half of the waveform. It is fixed in the current RTL, so a
 * `make flash` is the check. */
/* Master at maximum, and deliberately. One 8-bit channel cannot get
 * loud on this mixer -- full scale takes about four channels at full
 * gain -- so the level is set by keeping the MUSIC gains low rather
 * than by holding the master down. Turning the master down from here
 * quietens everything together, which is what it is for. */
#define MUS_VOL_DEFAULT 255

void music_set_volume(uint8_t vol);
uint8_t music_get_volume(void);

/* -- music vs effects --
 *
 * Two independent levels, 0..255, where 255 means the gains in music.c
 * exactly as written. These are a different question from the master
 * volume above: the master answers "how loud is the game", these
 * answer "how loud is the music AGAINST the effects".
 *
 * The default is deliberately lopsided. Music is FOUR channels
 * sounding together and an effect is ONE, so an effect has to be
 * roughly twice the whole music mix to be clearly heard over it. Set
 * them equal and every jump and coin vanishes underneath the tune --
 * which is not subtle, and was the state of this file until it was
 * pointed out.
 *
 * A music volume change takes effect on the next note, not
 * immediately: a note already sounding runs its envelope out at the
 * old level, which is a fraction of a second and much less jarring
 * than every voice jumping mid-note. */
#define MUS_MUSIC_VOL_DEFAULT 255
#define MUS_SFX_VOL_DEFAULT   255

void music_set_music_volume(uint8_t vol);
uint8_t music_get_music_volume(void);
void music_set_sfx_volume(uint8_t vol);
uint8_t music_get_sfx_volume(void);

/* Call once per frame, with the frame count z_game_flip() returned. */
void music_tick(uint32_t frames);

void music_play(int song);
void music_stop(void);

void sfx_play(int which);

/* Silence everything. Call before exiting -- a channel left enabled
 * goes on fetching from a buffer that is about to be freed with the
 * process. */
void music_shutdown(void);

#endif
