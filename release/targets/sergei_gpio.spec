# Sergei with four GPIO pins on its PMOD connector, and no optical
# audio output.
#
# `AUDIO_SPDIF is removed, and that is the whole trade rather than a
# side effect. PMOD pin 1 is ball A13, which is also the optical
# S/PDIF output -- boards/sergei_ml1.lpf has been putting AUD_OPTICAL
# there all along. One ball, two possible functions.
#
# AND THIS BOARD HAS NO OTHER AUDIO OUTPUT. `AUDIO and `AUDIO_MIXER
# stay defined, so the mixer still runs and sw/apps/play still thinks
# it is playing -- the samples simply have nowhere to go. That is
# worth knowing before flashing this over a working machine: it is not
# "audio gets quieter", it is "audio goes nowhere", with nothing on
# screen to say so.
#
# The obvious alternative -- keep S/PDIF and take only pins 2-4 -- is
# not offered, because a port whose pin 0 is missing needs a different
# shape from one that is merely short: the pins would be bits 1-3 with
# a hole at the bottom, and gpio4.spec's "bits 4-7 have no pins" note
# would become "bits 0 and 4-7", which is the point at which the
# per-port pin mask this deliberately avoids starts being the right
# answer. Three pins is also not much: not enough for SPI with a chip
# select, and I2C plus one spare.


# -- PIN 1 IS NOT A GENERAL-PURPOSE PIN, and this was measured --
#
# A13 reaches the connector THROUGH the optical transmitter's series
# resistor, and the transmitter stays on the net whether or not
# `AUDIO_SPDIF is built -- removing the define frees the FPGA pin, not
# the board wiring.
#
# So bit 0 is:
#
#   AS AN INPUT: useless. It reads 0 always. The transmitter's load
#     beats the ~50k internal pull-up, so the pin cannot go high and
#     the pull-up cannot help whatever you connect there either.
#     Confirmed on hardware: with all four pins as inputs and nothing
#     attached, the port reads 14 (0b1110) -- bits 1-3 floating high
#     on their pull-ups, bit 0 held down. Jumpering P1 to ground
#     changes nothing, because it was already there.
#
#   AS AN OUTPUT: works, into a high-impedance input. It also fires
#     the optical LED whenever it is driven high. Harmless, and
#     visible, and a useful reminder of what the pin is.
#
# Bits 1, 2 and 3 (P2/R12, P3/T13, P4/T14) are ordinary GPIO and were
# confirmed the same way -- jumpering each to ground clears exactly
# its own bit.
#
# So: THREE usable pins plus one output-only pin. Not enough for SPI
# with a chip select; enough for I2C with a spare, if SCL and SDA go
# on bits 1-3 -- open drain needs a pin that can read back, which
# bit 0 cannot do.

description = Sergei ML1 + 4-pin GPIO (no optical audio)

base  = sergei_ml1
pmods = gpio4@a

defines = -AUDIO_SPDIF
