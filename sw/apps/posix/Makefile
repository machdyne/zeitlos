# sw/apps/posix -- build rules are shared: sw/common/app.mk, docs/build.md.

APP = posix
APP_MAIN_RULE = 0
# posix -- a Unix-shaped shell for Zeitlos, as a port provider.
# Phase 4 of docs/posix.md. Modelled on ../repl/Makefile, which is the
# reference for this shape, and far shorter than it: there is no
# Scheme, no editor and no pager here.

APP_CFLAGS = -I../../common

# -Werror applies to THIS APP'S OWN sources and nothing else.
#
# It was on CFLAGS at first, which also compiles the sw/common objects
# below -- and those have warnings that vary by toolchain version
# (zeitlos.c's _exit() is `noreturn` and does return; _read() has a
# maybe-uninitialized that some GCCs see and others do not). An app
# has no business failing the build over warnings in code it does not
# own, and making every app in the tree warning-clean on every
# toolchain is a different job from writing this one.
#
# So: shared code builds with warnings visible, this app's code builds
# with them fatal. The reason -Werror is here at all is on record in
# docs/zcc.md -- zcc shipped for several days with three implicit
# declarations nobody read, and a newer GCC turned them into errors.
OWN_CFLAGS = $(CFLAGS) -Werror

# Objects, and why each is linked:
#
#   zconnect.o: For z_conn_handoff() -- the Z_TERM_SET_PORT message that hands a term
#   window to a child and takes it back. Same helper `repl` uses for
#   `telnet`; see posix.h's PX_TTY_TAG.
#
#   zdns.o: `port` goes through z_conn_prepare(), which also knows telnet and ssh
#   targets and so their hostname lookup -- as repl's does.
#
#   zwin.o: zwin is here for z_launch_arg_set() alone -- the mechanism the `run`
#   builtin uses to hand a command line to the program it starts. It
#   drags in zgfx/zfont_data, which --gc-sections then drops almost all
#   of; without section GC this app is several KB larger for one call.
# zgz.o, zinflate.o: zcat and gunzip (sw/common/zgz.c over zinflate.c)
OBJS = zeitlos.o zobj.o zport.o zline.o zfsapp.o zfsutil.o zwin.o zgfx.o zfont_data.o \
	zconnect.o zdns.o zinflate.o zgz.o \
	vfs.o sh.o main.o

vfs.o: vfs.c posix.h
	$(CC) $(OWN_CFLAGS) -c vfs.c -o vfs.o
sh.o: sh.c posix.h
	$(CC) $(OWN_CFLAGS) -c sh.c -o sh.o
main.o: main.c posix.h
	$(CC) $(OWN_CFLAGS) -c main.c -o main.o

# The shell's own test suite, built with the HOST compiler.
#
#   make test
#
# sh.c and vfs.c contain no port, message or window code at all -- that
# is main.c's whole job -- so they can be driven directly against a
# host filesystem. See tests/run.sh for why that split was worth
# arranging rather than testing this only on hardware.
test:
	@$(MAKE) -C tests run

.PHONY: test

clean: clean-tests
clean-tests:
	@$(MAKE) -C tests clean
.PHONY: clean-tests

include ../../common/app.mk
