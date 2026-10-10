# sw/apps/fs -- the filesystem, as a driver. docs/fs_driver.md.
#
# FatFs and the card's SPI driver live here (fatfs/), not in the
# kernel. A core app: in the flash archive, started first (sw/os/sh.c).

APP = fs
ZFMT = 1
# (fs itself never calls printf: see say() in fs.c)
# Heap and stack: the 32KB bulk reply buffer (only without the grant
# window) and a FatFs call's stack. Volumes cost no heap -- the RAM
# disk is the kernel's.
APP_STACK = 64K
APP_DRIVER = 1
# fatfs/sdmm.c includes zeitlos.h and zsoc.h by bare name
APP_CFLAGS = -I../../common

SRC_DIRS = fatfs

OBJS = zeitlos.o zobj.o zusbc.o ff.o ffunicode.o sdmm.o fs.o

include ../../common/app.mk
