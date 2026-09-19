# Lakritz with a Katze RMII ethernet PMOD in port A.
#
# Katze is a PHY and nothing else, so unlike Langkatze the MAC is in the
# fabric: rtl/ethmac_rmii.v, the one mozart_ml1 and sergei_ml1 use. The
# PMOD spec adds `ETH_RMII; everything below is about fitting that MAC
# into an ECP5-25F, which is the part that took the work. docs/katze.md
# has the full measurements.
#
# -- -SPI_ETH --
#
# The base board defines `SPI_ETH because boards/lakritz_v0.lpf assumes
# a Langkatze in port A. Katze occupies that port instead, so those
# constraints are released and the define has to go with them -- same
# reasoning as lakritz_gpio. Leaving it would also mean two NICs, which
# release/lib/spec.py's nic() rejects.
#
# -- Block RAM is the constraint --
#
# The plain Lakritz build uses 55 of the 25F's 56 DP16KD (measured,
# nextpnr). 40 of those are VRAM. The MAC's smallest block RAM
# footprint is 3: two receive slots and the transmit buffer, one EBR
# each. So two blocks have to come from somewhere, and they come from
# the two places that give them up most cheaply:
#
#   ICACHE_KB=2         icache 3 EBR -> 2   (measured, synth_ecp5)
#   AUDIO_FIFO_LOG2=9   audio  2 EBR -> 1   (measured, synth_ecp5)
#
# which lands at exactly 56 of 56.
#
# ICACHE_KB=2: half the instruction cache. Fetches from SDRAM miss
# more often; nothing breaks. docs/icache.md has the hit-rate method.
#
# AUDIO_FIFO_LOG2=9: 512 stereo frames instead of 1024 -- 11.6ms at
# 44.1kHz. rtl/sysctl.v's note says 128 was too short for a player that
# loses the CPU for two or three 1.365ms ticks; 512 is still about three
# times that. Software reads the depth from CONFIG (z_audio_depth()),
# so nothing needs rebuilding. That note's claim that 512 and 1024 cost
# the same two blocks does not hold for this FIFO: its ports are one
# write and one read, which yosys maps to the 36-bit PDPW16KD mode.
#
# -- The alternative that was measured and rejected --
#
# rtl/ethmac_rmii.v can put its receive buffer in LUT RAM instead
# (`ETH_RXBUF_LUTRAM), which needs 1 EBR and no trades. On this build
# it took TRELLIS_COMB from 19191 to 24766 of 24288 -- 101%, does not
# place. The option stays in the MAC for a board with LUTs to spare.
#
# -- Two receive slots, not four --
#
# Half the frames of the ML1 boards, so a burst arriving faster than
# `net` drains it drops sooner here; rx_drop_count (STATUS[7:4]) counts
# it. Software is told: the MAC reports its slot count in STATUS[15:12]
# and sw/apps/net sizes the TCP receive window from that -- one
# 536-byte segment here, three on a four-slot MAC. Advertising more
# than the MAC can hold is what turns "slower" into "dropping".
#
# A denser VRAM is the real fix, and would give this target four slots
# and its full icache and audio FIFO back. See docs/katze.md.
#
# -- The console --
#
# Not an issue. `USB_CDC is on for Lakritz, and the `undef at the bottom
# of rtl/boards.vh removes `UART0 -- the console is the USB-C socket on
# every Lakritz build, so Katze taking all eight PMOD pins costs nothing
# there.

description = Lakritz + Katze RMII Ethernet PMOD

base  = lakritz
pmods = katze@a

defines =
	-SPI_ETH
	ETH_RX_SLOTS=2
	ICACHE_KB=2
	AUDIO_FIFO_LOG2=9
