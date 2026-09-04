# Obst with the console on PMOD A and a second, general-purpose serial
# port on PMOD B.
#
# The configuration for talking serial to something else while keeping
# a console -- a microcontroller, a modem, a GPS, another Zeitlos
# machine. `sh` and the kernel still print to PMOD A exactly as usual;
# PMOD B is UART1, owned by whatever process opens it (sw/apps/serial
# by convention, which `term` can then connect to as a port -- see
# docs/uart1.md).
#
# `SPI_ETH is removed for the same reason obst_uart_gpio removes it:
# Obst has two PMOD connectors, the console is on one, and the
# Langkatze ethernet PMOD is on the other. There is no third socket.
# The choice is ethernet OR a second serial port, and only a spec that
# can say `-SPI_ETH` can express that -- an additive -D on the yosys
# command line cannot remove a define.
#
# -- This target and obst_uart_gpio are alternatives, not a progression
#
# Both take PMOD B. If you want GPIO and a second UART at the same time
# on Obst, you cannot have it: that is four pins for the UART's two
# plus flow control it does not use, against eight for a GPIO port, in
# one eight-pin connector. A combined PMOD spec could split the
# connector -- UART1 on pins 2 and 3, six GPIO pins on the rest -- and
# that is a real option worth building if anyone wants it. It is not
# this target, because a GPIO port with two of its eight pins missing
# needs its own explanation everywhere a port count is reported, and
# nobody has asked yet.

description = Obst + USB-UART PMOD + second UART

base  = obst
pmods = usbuart@a usbuart1@b

defines = -SPI_ETH
