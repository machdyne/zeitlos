# Sergei ML1 with the narrow GPIO port instead of optical audio.
#
# Named for the board it is a variant OF -- sergei_ml1_gpio rather
# than sergei_gpio -- because "sergei" alone is not a board, and the
# release ships both this and the plain sergei_ml1.
#
# THE TRADE IS ALL AUDIO, NOT JUST THE OPTICAL PORT. boards/
# sergei_ml1.lpf puts AUD_OPTICAL and PMOD_A1 on the SAME BALL, A13 --
# the board wires one pad to both -- so four GPIO pins and an S/PDIF
# transmitter cannot coexist. `-AUDIO_SPDIF` is what releases A13, and
# Sergei has no other audio output: this build is silent.
#
# It is silent WITHOUT SAYING SO. `AUDIO and `AUDIO_MIXER stay defined
# because the mixer and the sample path are still built; only the
# transmitter that puts them on a wire is gone. sw/apps/play runs,
# reports a duration, advances its position, and produces nothing. See
# docs/gpio.md, "Pin 1 is also the optical S/PDIF output".
#
# Three of the four pins are usable for input. Bit 0 is on A13 and is
# output-only -- again see docs/gpio.md.

description = Sergei ML1 + GPIO (no audio)

base  = sergei_ml1
pmods = gpio4@a

defines = -AUDIO_SPDIF
