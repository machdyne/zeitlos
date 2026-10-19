# sw/apps/print -- build rules are shared: sw/common/app.mk, docs/build.md.
#
# print: the print service (docs/print.md). A card app, started by an
# app that prints (sw/common/zprint.h) with a job file in /tmp; it lays
# the pages on sheets and sends them -- PWG raster over IPP, PostScript
# to port 9100, or a PDF or PostScript file.
#
#   make              the app
#   make test         the host tests: job reading, PWG, PDF, PostScript,
#                     IPP against ippeveprinter (when installed)

APP = print
APP_STACK = 32K

APP_CFLAGS = -DZ_IMG_HAVE_PNG=1

OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zwin.o \
	zfsapp.o zdns.o zport.o zcfg.o \
	zimg.o zinflate.o zcanvas.o zcfont.o zpage.o zpaper.o zpool.o \
	out.o pwg.o job.o raster.o vec.o ipp.o res.o run.o print.o

include ../../common/app.mk

# -- host tests --------------------------------------------------------------

HOSTCC ?= cc
COMMON = ../../common
HOST_CFLAGS = -std=gnu99 -g -O1 -Wall -Wextra -fsanitize=address,undefined \
	-fno-sanitize=shift -I. -DZ_IMG_HAVE_PNG=1
HOST_SRC = out.c pwg.c job.c raster.c vec.c ipp.c res.c run.c \
	$(COMMON)/zpool.c $(COMMON)/zpaper.c $(COMMON)/zpage.c $(COMMON)/zcanvas.c \
	$(COMMON)/zcfont.c $(COMMON)/zfont_data.c $(COMMON)/zimg.c $(COMMON)/zinflate.c

/tmp/print_host: tests/print_host.c tests/host_net.c $(HOST_SRC) *.h
	$(HOSTCC) $(HOST_CFLAGS) -o $@ tests/print_host.c tests/host_net.c $(HOST_SRC)

/tmp/test_print: tests/test_print.c $(HOST_SRC) *.h
	$(HOSTCC) $(HOST_CFLAGS) -o $@ tests/test_print.c $(HOST_SRC)

host: /tmp/print_host

test: /tmp/test_print /tmp/print_host
	/tmp/test_print ../../..
	sh tests/run_tests.sh

CLEAN_EXTRA = tests/out
