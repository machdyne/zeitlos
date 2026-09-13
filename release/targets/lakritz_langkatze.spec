# Lakritz with a Langkatze SPI ethernet PMOD in port A.
#
# Same board as lakritz_gpio with a different PMOD in the same socket,
# which is exactly why `-UART0` is here.
#
# boards/lakritz_v0.lpf puts UART0_TX on B12 and UART0_RX on B13 --
# PMOD_A2 and PMOD_A3. Langkatze needs those two balls for MOSI and
# MISO, so this build cannot also have a serial console: the pins are
# the pins. The generator releases the base .lpf's UART0 constraints
# automatically (the port is occupied by something else now), and this
# line removes the ports to match, since a declared port with no
# constraint is a place-and-route failure.
#
# WHAT THIS COSTS: no serial console. The machine still has HDMI, USB
# keyboard and mouse, microSD and now networking, so it is a complete
# system -- just not one you can talk to over a UART.
#
# WHAT MAKES IT SAFE: rtl/uart_null.v. Without `UART0 the 0xf000_0000
# window is answered by a phantom 16550 that reports a transmitter
# always ready and a receiver never holding data, so sw/bios/bios.c's
# putchar() and sw/os/uart.c's loops all run to completion and write
# into nothing. Before that block existed this configuration hung on
# the first character of the boot banner. Z_FEATURE_UART0 is clear on
# this build for software that wants to say so out loud.

description = Lakritz + Langkatze SPI Ethernet PMOD

base  = lakritz
pmods = langkatze@a

# No -UART0 here either: `USB_CDC is on for Lakritz and the `undef at
# the bottom of rtl/boards.vh drops `UART0. The console is the USB-C
# socket on every Lakritz build now.
#
# No +SPI_ETH either -- the langkatze PMOD spec adds it, and the base
# board already defines it because boards/lakritz_v0.lpf constrains the
# ethernet pins on PMOD A. This target says where the PMOD is plugged
# in; the define follows from that.
