# USB-UART PMOD carrying UART1 -- a general-purpose serial port, not
# the console.
#
# Same two pins in the same places as usbuart.spec; the only difference
# is which UART they belong to and the `+UART1` that instantiates a
# second 16550 to drive them. rtl/sysctl.v has had the block, the
# decode and the pins behind `ifdef UART1 all along -- it was only ever
# reachable on the ULX3S, where it is soldered to the on-board ESP32.
# This makes it reachable as a port you can plug something into.
#
# -- UART0 vs UART1, and why they are not interchangeable --
#
# UART0 is the console: sw/bios/bios.c writes to it before anything
# else exists, the kernel prints to it, and `sh` reads from it. Nothing
# else may own it.
#
# UART1 has no such role. It is a serial port for talking to something
# else -- a modem, a microcontroller, a GPS, another Zeitlos machine --
# and it belongs to whatever process opened it (sw/apps/serial by
# convention). See docs/uart1.md.
#
# -- Which is why the feature bit is not set on a ULX3S --
#
# rtl/csrs.vh sets FEATURES2 bit 1 for `UART1 only when `ESP32_LINK is
# absent. A ULX3S has a UART1 and always has, but it is soldered to the
# ESP32 with no header and no second owner, so a bit saying "this board
# has a serial port you can open" would be true of the gateware and
# false of the board. Software finds that one through
# Z_FEATURE_ESP32_LINK, as it always did.
#
# -- Flow control --
#
# Not wired, on either UART. The 16550 has CTS/DSR/RI/DCD inputs and
# rtl/sysctl.v ties all four to 1 (asserted). Pins 1 and 4 of the
# connector are therefore free, and a target could hand them to a GPIO
# port -- but nothing in this tree does that yet, and combining two
# functions in one connector needs a spec that describes the whole
# connector rather than two that each claim it.

description = USB-UART PMOD (UART1, general purpose)

pins =
	2=UART1_TX
	3=UART1_RX

io_type = LVCMOS33

defines = +UART1

notes = A second serial port on this PMOD, independent of the console. No flow control.
