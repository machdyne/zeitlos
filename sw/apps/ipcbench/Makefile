# sw/apps/ipcbench -- build rules are shared: sw/common/app.mk, docs/build.md.
#
# Phase 0 of docs/userspace_drivers.md: IPC round trips, wakeup
# latency and filesystem costs, measured on the machine.

APP = ipcbench
ZFMT = 1
# 1MB: the whole-file read test holds a 512KB buffer
APP_STACK = 1M
# A driver (ZEXE flags bit 15): the echo copy opens the grant window
# onto the master, as a filesystem driver would onto its client.
APP_DRIVER = 1
# Straight to fs, as an app built this way talks to it (zfsapp.c)
APP_CFLAGS = -DZFSAPP_DIRECT

OBJS = zeitlos.o zobj.o zfsapp.o zfsc.o ipcbench.o

include ../../common/app.mk
