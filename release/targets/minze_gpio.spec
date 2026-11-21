# Minze -- GPIO on its one PMOD port.
#
# The console is USB CDC-ACM on the USB-C socket, so the PMOD is free.
# This is exactly what the board's own block and boards/minze_v1.lpf
# describe; `zrelease check` reports the generated constraints as
# identical to the board file's.

description = Minze + GPIO

base  = minze
pmods = gpio@a

# The CPU is zeitlos32ps (docs/zeitlos32ps.md): about 2.7x zeitlos32's
# speed in less room than picorv32. Only release builds pick it here;
# `make BOARD=x` still builds picorv32 until rtl/boards.vh changes.
defines = +CPU_ZEITLOS32PS
