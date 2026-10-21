# sw/apps/snake -- build rules are shared: sw/common/app.mk, docs/build.md.
#
# The classic snake game, on the arcade runtime (sw/common/games/
# zarcade.[ch], docs/arcade.md) and its sound engine (zsfx.[ch],
# docs/sfx.md). docs/snake_app.md.

APP = snake
APP_LISTING = 0

SRC_DIRS = $(COMMON_DIR)/games

# Objects, and why each is linked:
#
#   zgfx, zcolor           the blitter and game-mode colour
#   zwin, zwidget, zspeak,
#   zdialog                the window and its F1 help box
#   zfsapp                 the high-score file and settings.txt
#   zcfg                   apps.arcade.skill, the default skill
#   zarcade, zarcade_sys   the arcade runtime: window or full screen
#   zsfx                   effects and music on the hardware mixer
OBJS = zeitlos.o zobj.o zgfx.o zcolor.o zfont_data.o zwin.o zwidget.o \
	zspeak.o zdialog.o zfsapp.o \
	zarcade.o zarcade_sys.o zsfx.o zcfg.o \
	sn_game.o sn_levels.o sn_draw.o sn_sound.o snake.o

# -- host-side tests: the rules, and pictures of every screen --

test:
	$(MAKE) -C tests

render:
	$(MAKE) -C tests render

clean: clean-tests
clean-tests:
	$(MAKE) -C tests clean

.PHONY: test render clean-tests

include ../../common/app.mk
