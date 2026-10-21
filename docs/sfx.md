# Game sound: zsfx

`sw/common/games/zsfx.[ch]`: sound effects and music for games, on the
hardware mixer ([audio.md](audio.md), "The hardware mixer"). Used by
`snake`, and by the arcade runtime ([arcade.md](arcade.md)), whose
`ar_frame()` steps it once a frame and whose M key turns the music on
and off.

It is mausrun's old `music.c` made general -- and mausrun itself plays
on it now: the same trick (an instrument is one looped cycle of a
waveform, and pitch is the rate it is played at), the same ASCII scores, the same per-frame envelopes -- plus effects
that are little programs instead of pre-rendered buffers, so they sweep,
step through arpeggios and pan, and cost no memory each.

## Channels

| Channels | Used for |
|---|---|
| 0..3 | music: a song has up to four channels |
| 4..6 | effects |
| 7 | effects, unless speech is running (`Z_AUDIO_CH_SPEECH`, [tts.md](tts.md)) |

Like `track`, `play` and `midi`, a game takes the mixer while it runs
and clears the channels it uses -- but never channel 7 while the speech
service is up, and on the way out it leaves the mixer on if speech is
speaking through it. The DAC is enabled only if nothing has enabled it,
and its rate is left exactly as the board set it (an S/PDIF board needs
its own). The mixer's output scale is set to its top: one 8-bit channel
cannot get loud on this mixer, so the balance is set by keeping the
music gains low (as mausrun's old `music.h` did). The system volume -- wm's,
Super+Minus/Equal -- is untouched and is the knob.

## Waveforms

Built at startup, 64 samples a cycle, peaks of 100 so several channels
together do not clip the sum: square, 25% and 12.5% pulse, triangle,
saw, sine, and a 4KB window of 16-bit LFSR noise (whose "pitch" is its
playback rate over 64).

## Effects

An effect is up to eight segments. Each holds one waveform for some
frames and slides linearly in pitch (Hz) and level (0..255):

```c
static const zsfx_seg_t eat_seg[] = {
    { ZSFX_SQUARE, 3,  659,  659, 200, 170 },    /* E5 */
    { ZSFX_SQUARE, 3,  784,  784, 180, 150 },    /* G5 */
    { ZSFX_SQUARE, 6, 1047, 1047, 160,   0 },    /* C6, fading */
};
static const zsfx_fx_t eat = { 2, 3, eat_seg }; /* priority, segments */

zsfx_play(&eat, zsfx_pan_x(x));                 /* -128 left .. 127 right */
```

Each frame a running effect writes its channel's step and gain -- EN
without TRIG, so the waveform keeps its phase and a sweep is a sweep,
not a string of clicks. A segment that changes waveform restarts the
channel. A new effect takes a free effect channel, or else the oldest
effect of no higher priority; it never takes a music channel.

Effects are panned: snake puts each sound where it happens on screen.

## Music

Patterns of 16 rows, one string per row, a three-character cell per
channel, as in mausrun:

```c
static const char *const rows[] = {
    "E-5 B-4 E-2 K--",
    "--- --- --- H--",
    "G-5 E-4 E-3 H--",
    ...
};

const zsfx_song_t song = {
    528, 4,                                /* rows a minute, channels */
    {
        { ZSFX_PULSE25, 22, 238, 4 },     /* wave, gain, decay, vibrato */
        { ZSFX_SQUARE,  10, 225, 0 },
        { ZSFX_TRI,     30, 248, 0 },
        { ZSFX_DRUMS,  255,   0, 0 },
    },
    rows, 4, "01230123"                    /* patterns, order */
};
zsfx_music_play(&song);
```

| Cell | Means |
|---|---|
| `C-4`, `C#4` | a note; octave 4 holds A-4 = 440Hz |
| `---` | hold |
| `===` | note off |
| `K--` `S--` `H--` `O--` | on a `ZSFX_DRUMS` channel: kick, snare, closed hat, open hat |

The drums are effect programs run on the music channel: a kick is a
triangle falling from 160Hz to 45Hz, a snare and the hats are noise
with a fast decay. A note's level decays by `decay`/256 each frame; a
small triangle vibrato (`vib`) keeps a lead alive. The gains are the
balance between the channels and should sum to well under the effects'
level -- four music channels against one effect channel
([mausrun.md](mausrun.md)'s lesson: equal levels bury every effect).

Tempo is rows a minute and is converted against the display's real
frame rate, so a song is the same speed on a 50Hz PAL composite board.
`zsfx_music_tempo(percent)` scales it; `zsfx_music_intensity()` (below)
sets it with the arrangement. `zsfx_music_rows()` counts the rows played since the song
started (even while muted), a beat to pulse a picture to: blocks
brightens its theme colour every four rows. `zsfx_music_enable(false)` silences the music and remembers
it; `zsfx_suspend(true)` holds everything (a pause, the help box).

On a board without the mixer every call is a no-op and the game plays
silently; `zsfx_init()` returns false so the game can say so.

### Calm first

The arcade games' tunes were written for the game at full tilt, and
heard from the first second they were too fast and too busy -- the
drums especially -- for someone just starting. So a song is now
*arranged by the game as it goes*:

```c
zsfx_music_layers(channels, drums);   /* which channels, which drums */
zsfx_music_intensity(level);          /* 0 .. ZSFX_INTENSITY_FULL (10) */
```

`zsfx_music_layers()` picks the song channels that are heard (bit `ch`)
and, on a drum channel, the drums (bit 0 kick, 1 snare, 2 closed hat,
3 open hat). A channel left out stops at once and the song keeps its
place, so bringing it back lands in time. The setting survives
`zsfx_music_play()`.

`zsfx_music_intensity()` sets both at once, and the tempo, for a song
laid out as the arcade games' are (channel 0 the tune, 1 the harmony, 2
the bass, 3 the drums):

| Level | Heard | Tempo |
|---|---|---|
| 0 | tune and bass | 70% |
| 1 | + harmony | 74% |
| 2 | + closed hat | 78% |
| 3 | + open hat | 82% |
| 4, 5 | + kick | 86%, 90% |
| 6 .. 10 | + snare: the whole song | 94% .. 110% |

snake takes the level from the snake's speed, blocks from its level (from
0, 4 or 7 by skill, so normal starts near the pace it always had) and
bricks from the round; all three play their title tune at 3. mausrun from its
skill and level (1, 3 or 5 to start, a step every two levels). A game
that never calls either gets every channel at the tempo as written.

### Master volume

```c
zsfx_master_volume(v);     /* 0..255, everything; 255 (the default) as written */
zsfx_master_get();
```

One knob for a game's own volume keys (mausrun's `-` and `=`): every
note and every gain zsfx writes, music and effects alike, is scaled by
it as it is written, so the balance between them stays as it was. It is
zsfx's own scale, not the mixer's output register, so the system volume
(wm's) is still untouched.

## Testing

`make -C sw/common/games test-arcade` builds zsfx with `ZSFX_HOSTED`,
where the four hardware calls are a recording: cells and orders parse,
an effect starts a note and its next waveform another, more effects
than channels steal the oldest, a song's first row is three notes and a
kick, music off and suspend are silent, layers and intensities leave
out what they should.
