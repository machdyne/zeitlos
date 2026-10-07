# sw/apps/space3d -- build rules are shared: sw/common/app.mk, docs/build.md.

APP = space3d

# -march/-mabi on the LINK line too, not just for compiling: gcc uses
# them to pick which multilib's libc/libgcc to search, and omitting
# them silently falls back to the toolchain default -- which may assume
# an ISA the configured PicoRV32 doesn't have. See docs/user_input.md,
# "Debugging notes".

OBJS = zeitlos.o zobj.o zgfx.o zcolor.o zfont_data.o zwin.o space3d.o

include ../../common/app.mk
