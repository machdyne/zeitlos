# USB-UART PMOD -- the serial console the top-level README assumes
# ("minicom -D /dev/ttyACM0 -b 1000000").
#
# Pin functions are given as PMOD pin numbers, not FPGA balls: that is
# the whole point of the split. This file says what the PMOD does with
# each pin, a board spec says which ball each of its PMOD port's pins
# lands on, and a target spec plugs one into the other. The same two
# lines below are correct on every board that has a PMOD port.
#
# Inferred rather than invented: boards/lakritz_v0.lpf puts UART0_TX on
# B12 and UART0_RX on B13, which are Lakritz's PMOD_A2 and PMOD_A3. So
# the board .lpf has been hardcoding "a USB-UART PMOD in port A" all
# along -- this spec just says it out loud, in a form another board can
# reuse.

description = USB-UART PMOD (serial console)

pins =
	2=UART0_TX
	3=UART0_RX

io_type = LVCMOS33

notes = Assumes a USB-UART PMOD on the console port.
