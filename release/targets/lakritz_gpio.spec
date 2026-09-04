# Lakritz with eight GPIO pins on its one PMOD connector, and no
# serial console at all.
#
# This is the target that most needs its reasoning written down,
# because "no console" reads like a mistake.
#
# Lakritz has exactly ONE PMOD port. boards/lakritz_v0.lpf puts
# UART0_TX on B12 and UART0_RX on B13, which are PMOD_A2 and PMOD_A3 --
# so the board has been assuming a USB-UART PMOD in that socket all
# along (see hw/pmods/usbuart.spec, which just says it out loud). A
# GPIO port needs all eight pins of the connector. There is no
# arrangement in which both fit.
#
# So `UART0 comes out. What that means in practice:
#
#   - rtl/sysctl.v instantiates rtl/uart_null.v in its place, which
#     answers the 0xf000_0000 window with a transmitter that is always
#     ready and a receiver that never has data. That is not a nicety:
#     sw/bios/bios.c's putchar() spins on the LSR, and an undecoded
#     address gets no ack on this bus, so without the null UART the
#     machine would hang on the first character of the boot banner.
#     See uart_null.v's own header.
#
#   - Everything that would have gone to the console goes nowhere.
#     Boot messages, kernel diagnostics, panics. On a board with no
#     serial port that is not a loss of information so much as a
#     relocation of it: the machine comes up on HDMI with a USB
#     keyboard and a window manager, and `repl` in a `term` window is
#     the shell.
#
#   - rtl/csrs.v's UART0 feature bit goes clear, so software that
#     wants to TELL the user there is no console can (sw/common/
#     zsoc.h, bit 12). Nothing has to be rebuilt differently.
#
# The eight balls the console used to share are now GPIO0[1] and
# GPIO0[2] among others; the generator drops the base .lpf's UART0
# constraints because gpio has taken the port.
#
# If you want both a console and GPIO on Lakritz, the answer is not
# this target -- it is a second board, or Obst, which has two
# connectors (see obst_uart_gpio).

description = Lakritz + GPIO (no serial console)

base  = lakritz
pmods = gpio

defines = -UART0
