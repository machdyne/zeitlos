# Lakritz with a Katze RMII ethernet PMOD in port A.
#
# Katze is a PHY and nothing else, so unlike Langkatze the MAC is in the
# fabric: rtl/ethmac_rmii.v, the one mozart_ml1 and sergei_ml1 use. The
# PMOD spec adds `ETH_RMII. docs/katze.md has the details.
#
# -- -SPI_ETH --
#
# The base board defines `SPI_ETH because boards/lakritz_v0.lpf assumes
# a Langkatze in port A. Katze occupies that port instead, so those
# constraints are released and the define has to go with them -- same
# reasoning as lakritz_gpio. Leaving it would also mean two NICs, which
# release/lib/spec.py's nic() rejects.
#
# -- ETH_RX_SLOTS=4 --
#
# The same four receive slots as the ML1 boards, so the TCP window
# `net` advertises is the same too. Said explicitly, as mozart_ml1 and
# sergei_ml1 do in rtl/boards.vh, rather than left to the MAC's default.
#
# The MAC costs 5 block RAMs at this size (4 slots + the TX buffer).
# That fits because VRAM is 20 blocks rather than 40 since
# rtl/mem/vram.v gained its no_rw_check attribute: this build uses 40
# of the 25F's 56. Before that fix the board had ONE block to spare,
# and this target had to halve the icache, the audio FIFO and the
# receive FIFO to fit -- see docs/katze.md's history note.
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
	ETH_RX_SLOTS=4
