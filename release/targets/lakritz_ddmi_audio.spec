# Lakritz with DDMI audio: sound goes out with the picture on the HDMI
# connector, as HDMI data islands between the video lines. No PMOD.
#
# This is a TEST target, not the one Lakritz ships with. The shipping
# Lakritz build is lakritz_gpio, with the sigma-delta output on the
# board's audio pins and plain DVI on the connector. This target exists
# so DDMI audio can be tried on the board it is most tight on; see
# docs/ddmi_audio.md.
#
# -- GPU_DDMI_AUDIO --
#
# Builds rtl/gpu/ddmi_island.v into the DDMI encoder: the island
# scheduler, the InfoFrames, the N/CTS clock-regeneration packet and the
# 16-entry sample FIFO into the pixel clock domain. It is still OFF at
# power-on and the output is bit-identical to plain DVI until software
# sets socctl's DDMI EN bit -- the OS does that at boot when the config
# says
#
#     system.video.ddmi_audio = yes
#
# and the shell's `ddmi on` does it by hand. A monitor that does not
# like data islands therefore still gets a picture from this build.
#
# The board spec's `AUDIO_EXACT stays: the HDMI sink regenerates the
# sample clock from N/CTS, which only agrees with the samples we send if
# the rate really is 48000 (or 44100, ...) Hz rather than the nearest
# divider of 48 MHz.
#
# -- -AUDIO_SD --
#
# The sigma-delta DACs go to make room. The frames the mixer produces
# are the same either way; they just are not ALSO dithered out of two
# pins. AUDIO_L and AUDIO_R are then not built, so their two LOCATEs in
# boards/lakritz_v0.lpf are dropped below rather than left as the
# "constraint for a port that does not exist" warning.
#
# -- -SPI_ETH --
#
# No PMOD: the second SPI master that talks to a Langkatze in port A is
# not built, and the five ETH_* constraints go with it. This is the
# part to revisit if the build turns out to have room -- `pmods = gpio`
# here (exactly as lakritz_gpio does it) is the first thing to try.
#
# The MPU stays, as on every Lakritz build; nothing here touches it.

description = Lakritz + DDMI audio (sound over HDMI, no PMOD, no sigma-delta)

base = lakritz

defines =
    GPU_DDMI_AUDIO
    -AUDIO_SD
    -SPI_ETH

lpf_drop = AUDIO_L AUDIO_R ETH_SS ETH_MOSI ETH_MISO ETH_SCLK ETH_INT
