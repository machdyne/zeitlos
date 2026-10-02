# Schoko -- Langkatze ethernet on PMOD A, GPIO on PMOD B.
#
# Name order is port order: the first PMOD named is in port A, the
# second in port B. obst_langkatze_gpio follows the same rule.
#
# The console is USB CDC-ACM on the USB-C socket (`USB_CDC in
# rtl/boards.vh), so both PMODs are free for something else. This is
# exactly what the board's own block and boards/schoko_v1.lpf
# describe; `zrelease check` reports the generated constraints as
# identical to the board file's.

description = Schoko + Langkatze ethernet + GPIO

base  = schoko
pmods = langkatze@a gpio@b
