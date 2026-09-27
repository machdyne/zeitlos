# zcc, the Zeitlos C compiler, built as an ORDINARY Zeitlos app.
#
# Same shape as every other app here: sw/common's runtime, newlib, and
# sw/common/riscv-app.ld with newlib's crt0 as the entry point.
#
# It was not always. The first version had its own linker script and a
# hand-written _start, linking libz instead of the C library -- not a
# design decision, but an artefact of a development machine whose
# RISC-V toolchain had no newlib and so could not link a normal app at
# all. Every assumption that entry stub made was untested and two of
# them crashed a real system. See port_dev.c's header.
#
# libz is still built, and zcc still needs it -- but as DATA. libz.bin
# is the runtime zcc embeds into the programs it produces, read from
# the card at run time and not linked into zcc. `make` here builds it
# so that a whole-tree build produces both.

# Build rules are shared: sw/common/app.mk, docs/build.md. What is
# particular to zcc is below: objects in build/, -Werror on its own
# sources only, and libz built alongside.

APP = zcc
APP_MAIN_RULE = 0

# -DZCC_HOSTED=0 on EVERYTHING, not just port_dev.c.
#
# It used to be on that one object, which was enough to select the
# right port and not enough for anything else. main.c also tests it --
# for the argv handling, where the host branch trusts the argc/argv
# main() was called with and the device has none to trust. That is the
# bug that crashed a machine once (see main.c), and it was quietly
# back the moment zcc became an ordinary app.
#
# A flag that selects "which machine is this for" belongs on the whole
# build, not on the file that most obviously needs it.
APP_CFLAGS = -I../../common -I. -DZCC_HOSTED=0

# -Werror on THIS APP'S OWN sources only. The sw/common objects below
# have warnings that vary by toolchain version (zeitlos.c's _exit() is
# `noreturn` and does return), and an app has no business failing the
# build over code it does not own. The reason -Werror is here at all
# is in docs/zcc.md -- zcc shipped for days with three implicit
# declarations nobody read.
OWN_CFLAGS = $(CFLAGS) -Werror

# Objects go in build/, NOT beside the sources.
#
# Makefile.host compiles main.c, lex.c, parse.c and the rest for the
# DEVELOPMENT HOST from this same directory. With both writing main.o
# here, whichever built last wins and the other link fails with "file
# in wrong format" -- which is at least loud. The quieter version of
# the same accident is a stale object that happens to be the right
# architecture.
#
# This was fixed once and reintroduced when zcc became an ordinary app,
# which is the argument for the comment rather than just the fix.
B = build
COMMON = zeitlos zobj zport zfsapp zwin zgfx zfont_data
OWN = main lex cpp parse emit util port_dev
OBJS = $(addprefix $(B)/,$(addsuffix .o,$(COMMON) $(OWN)))

# zwin for z_launch_arg_take() alone -- how `posix` hands zcc a command
# line. It pulls in zgfx/zfont_data, which --gc-sections then drops
# almost all of.
$(addprefix $(B)/,$(addsuffix .o,$(COMMON))): $(B)/%.o: ../../common/%.c | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

$(addprefix $(B)/,$(addsuffix .o,$(OWN))): $(B)/%.o: %.c zcc.h zcc_port.h | $(B)
	$(CC) $(OWN_CFLAGS) -c $< -o $@

$(B):
	mkdir -p $(B)

# The runtime zcc embeds into what it builds. A separate artefact with
# its own Makefile, not linked here -- see the header comment.
zcc: libz.data
libz.data:
	$(MAKE) -C libz

clean: clean-libz
clean-libz:
	$(MAKE) -C libz clean

.PHONY: libz.data clean-libz

CLEAN_EXTRA = $(B)

include ../../common/app.mk
