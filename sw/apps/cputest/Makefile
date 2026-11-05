# sw/apps/cputest -- build rules are shared: sw/common/app.mk, docs/build.md.
#
# A hardware check of the CPU's arithmetic (mul, mulh*, div*, rem*, the
# fixed-point products zgl uses) against slow references made of adds,
# shifts and compares, with the operands changing all the time and the
# results fed straight into loads, stores and branches. Not in
# sw/apps/Makefile's APPS and not on any card: `make -C sw/apps/cputest`
# and copy cputest.bin over; `run cputest [seconds]`.

APP = cputest
OBJS = zeitlos.o zobj.o cputest.o
APP_STACK = 64K

include ../../common/app.mk
