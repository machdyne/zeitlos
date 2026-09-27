# sw/apps/automate -- build rules are shared: sw/common/app.mk, docs/build.md.

APP = automate
APP_LISTING = 0

COMMON = zeitlos zobj zgfx zfont_data zwin zkbd zfsapp zcaption
OBJS = $(addsuffix .o,$(COMMON)) automate.o

$(addsuffix .o,$(COMMON)): %.o: ../../common/%.c
	$(CC) $(CFLAGS) -c $< -o $@

include ../../common/app.mk
