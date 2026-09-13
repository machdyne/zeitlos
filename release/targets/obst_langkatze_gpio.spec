# Obst -- ethernet on PMOD B, GPIO on PMOD A.
#
# The only Obst target. The console is USB CDC-ACM on the USB-C socket
# (`USB_CDC in rtl/boards.vh), which is what frees PMOD A: it used to
# carry UART0_TX/RX on D4 and B5, and a board whose console needs a
# PMOD cannot also put GPIO there.
#
# Declaring langkatze@b explicitly rather than leaning on the base
# .lpf's ethernet constraints: the port model releases a port's base
# constraints when something occupies it, so saying which PMOD is
# plugged in where is both the documentation and the thing that makes
# the constraint generation correct. A reader should not have to open
# boards/obst_v0.lpf to learn what is on B.

description = Obst + Langkatze ethernet + GPIO

base  = obst
pmods = langkatze@b gpio@a
