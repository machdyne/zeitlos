# sw/apps/zetta -- `zetta FILE`, the zetta editor in posix (docs/zetta.md).
#
#   make          the Zeitlos app (sw/common/app.mk)
#   make test     the session on the host: real files, the screen through
#                 the real terminal emulator
APP = zetta
APP_LISTING = 0
SRC_DIRS = .
OBJS = zeitlos.o zobj.o zport.o zwin.o zgfx.o zfont_data.o zfsapp.o zplat_zeitlos.o zetta.o edit.o zetta_zeitlos.o
override CFLAGS += -I../../common

.PHONY: test
test:
	cc -std=gnu99 -g -Wall -Wextra -fsanitize=address,undefined -o /tmp/test_zetta_file tests/test_edit.c edit.c \
		../../common/zetta.c ../../common/zvt100.c ../../common/zplat_posix.c
	/tmp/test_zetta_file

include ../../common/app.mk
