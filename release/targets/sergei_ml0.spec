# Sergei ML0 -- the complete system, with optical audio, on the Sechzig
# ML0 module (the ML1 on an LFE5U-25F).
#
# No PMOD. PMOD_A1 is ball A13, which is also the S/PDIF transmitter
# (see boards/sergei_ml1.lpf, which ML0 shares), so this target keeps
# A13 for audio and has no GPIO -- as sergei_ml1.

description = Sergei ML0

base = sergei_ml0

# The CPU is zeitlos32ps (docs/zeitlos32ps.md): about 2.7x zeitlos32's
# speed in less room than picorv32. Only release builds pick it here;
# `make BOARD=x` still builds picorv32 until rtl/boards.vh changes.
defines = +CPU_ZEITLOS32PS
