# Klinge -- headless, no PMOD sockets: the target is the board spec.
# Ethernet A and microSD A; the second PHY and slot are not built yet.

description = Klinge

base = klinge

# The CPU is zeitlos32ps (docs/zeitlos32ps.md): about 2.7x zeitlos32's
# speed in less room than picorv32. Only release builds pick it here;
# `make BOARD=x` still builds picorv32 until rtl/boards.vh changes.
defines = +CPU_ZEITLOS32PS
