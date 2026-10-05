# Minze -- GPIO on its one PMOD port.
#
# The console is USB CDC-ACM on the USB-C socket, so the PMOD is free.
# This is exactly what the board's own block and boards/minze_v1.lpf
# describe; `zrelease check` reports the generated constraints as
# identical to the board file's.

description = Minze + GPIO

base  = minze
pmods = gpio@a
