# Konfekt -- a complete system with no PMOD sockets, so the target is
# the board spec and nothing else.

description = Konfekt

base = konfekt

# The CPU is zeitlos32ps (docs/zeitlos32ps.md): about 2.7x zeitlos32's
# speed in less room than picorv32. Only release builds pick it here;
# `make BOARD=x` still builds picorv32 until rtl/boards.vh changes.
defines = +CPU_ZEITLOS32PS
