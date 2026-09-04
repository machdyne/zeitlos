# Obst with a USB-UART PMOD in port A and eight GPIO pins on port B.
#
# The configuration to reach for when Obst is talking to something
# else -- a sensor, a bus analyser, another microcontroller -- rather
# than to a network. Console on PMOD A exactly as usual, and PMOD B is
# eight bidirectional pins under software control (rtl/gpio.v,
# docs/gpio.md).
#
# `SPI_ETH is REMOVED, and that is the whole point of this target
# rather than an unfortunate side effect. Obst has two PMOD
# connectors; the console is on one and the Langkatze ethernet PMOD is
# on the other, so there is no third socket for GPIO to appear in. The
# choice is genuinely ethernet OR GPIO, and a spec that can express
# `-SPI_ETH` is the only thing that can say so -- an additive -D on the
# yosys command line cannot remove a define, which is exactly why this
# mechanism exists (see rtl/boards.vh's ZSPEC note).
#
# Removing it does two things. rtl/sysctl.v stops instantiating
# rtl/spim.v and stops declaring ETH_SS/ETH_MOSI/ETH_MISO/ETH_SCLK/
# ETH_INT, so the base .lpf's constraints on those balls become
# references to ports that no longer exist -- and the generator drops
# them anyway, because gpio takes over every ball in port B. And
# rtl/csrs.v's SPI_ETH feature bit goes clear, so sw/apps/net (if it is
# ever started here) reports no PHY and declines rather than hanging on
# registers that are not there.
#
# The DBG[7:0] LED bar in the base .lpf lands on these same PMOD B
# balls. It is not built on this board (`LED_DEBUG is commented out in
# rtl/boards.vh's BOARD_OBST block), so there is nothing to remove --
# but it is worth knowing that the LED bar and a GPIO port are the same
# eight pins in the same order, which is not a coincidence: gpio.spec's
# bit-to-pin mapping was taken from it.

description = Obst + USB-UART PMOD + GPIO

base  = obst
pmods = usbuart@a gpio@b

defines = -SPI_ETH
