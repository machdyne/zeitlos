# Obst -- ethernet on PMOD A, GPIO on PMOD B.
#
# Name order is port order: the first PMOD named is in port A, the
# second in port B (schoko_langkatze_gpio follows the same rule). Until
# this target was renamed-in-place it was the other way round --
# langkatze@b gpio@a -- which made the name read backwards.
#
# The only Obst target. The console is USB CDC-ACM on the USB-C socket
# (`USB_CDC in rtl/boards.vh), which is what frees PMOD A: it used to
# carry UART0_TX/RX on D4 and B5, and a board whose console needs a
# PMOD cannot also put the ethernet there.
#
# NOTE THIS IS NOT boards/obst_v0.lpf's ARRANGEMENT. A plain
# `make BOARD=obst` build still has the Langkatze on PMOD B, as that
# file has always constrained it. The port model releases every base
# constraint landing on an occupied port, so this target moves the five
# ETH_* signals to PMOD A and puts GPIO0[7:0] on PMOD B regardless of
# what the base file says -- `zrelease check` prints the delta.
#
# Moving a Langkatze from B to A is a re-plug, not a rebuild of the
# PMOD: same pins, same connector orientation, other socket.

description = Obst + Langkatze ethernet + GPIO

base  = obst
pmods = langkatze@a gpio@b
