# GPIO on a connector with only four signal pins.
#
# Same as gpio.spec in every respect except how many pins there are --
# see that file for the pin order, the pull-up choice and why "not a
# PMOD at all" is still described by a PMOD spec.
#
# For a 6-pin PMOD connector (Sergei). The gateware side needs
# `GPIO_PORT0_NARROW as well as `GPIO_PORT0: the port is declared four
# bits wide, because an unconstrained top-level IO is a hard
# nextpnr-ecp5 error and the flag that suppresses it would place the
# missing pins on whatever balls are free -- which on a populated
# board includes the SDRAM, the PHY and the SD card.
#
# SOFTWARE STILL SEES AN EIGHT-BIT PORT. DIR and OUT bits 4-7 exist
# and drive nothing; IN bits 4-7 read 0, where a real floating pin
# reads 1 because of the pull-up. Nothing reports the difference --
# a per-port pin-count register would be honest and is machinery on
# every board for one connector on one board. This note, and the
# release notes for the target, are the answer instead.

description = GPIO port (4 pins, bidirectional)

pins =
	1=GPIO0[0]  2=GPIO0[1]  3=GPIO0[2]  4=GPIO0[3]

io_type = LVCMOS33 PULLMODE=UP

defines = +GPIO_PORT0 +GPIO_PORT0_NARROW

notes = Four GPIO pins with weak internal pull-ups. Bits 4-7 of the port have no pins and read 0.
