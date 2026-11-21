# Mozart ML0 -- a complete system. No PMOD layer, because there are no
# PMOD sockets: the ethernet PHY, the audio DAC and the video output
# are all on the board, so there is exactly one Mozart configuration
# and it is the board spec.

description = Mozart ML0

base = mozart_ml0

# The CPU is zeitlos32ps (docs/zeitlos32ps.md): about 2.7x zeitlos32's
# speed in less room than picorv32. Only release builds pick it here;
# `make BOARD=x` still builds picorv32 until rtl/boards.vh changes.
defines = +CPU_ZEITLOS32PS
