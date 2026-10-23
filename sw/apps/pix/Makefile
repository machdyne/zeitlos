# sw/apps/pix -- build rules are shared: sw/common/app.mk, docs/build.md.
#
# The pixel editor: dock icons, fonts, sprite sheets (the pointer's
# too) and pictures, on the arcade runtime (sw/common/games/zarcade.h).
# docs/pix_app.md.

APP = pix
APP_LISTING = 0

# Documents live on the heap: a 1024 x 1024 picture, jp12's 7,000
# glyphs, a colour picture of up to 512 KB and as much again while an
# edit's undo step is taken whole (px_edit.h; it is then cut down to
# what changed, and the steps hold up to 512 KB).
APP_STACK = 2M

SRC_DIRS = $(COMMON_DIR)/games $(COMMON_DIR)/games/ui

# Objects, and why each is linked:
#
#   zgfx, zcolor           the blitter and game-mode colour
#   sui                    pix's boxes (open, save, prompts) and F1 help,
#                          on its own screen (the screen toolkit,
#                          docs/screen_ui.md)
#   zwin, zwidget, zspeak,
#   zdialog, zflist, zedit the window, and the runtime's desktop help box
#                          (zarcade_sys.c; unused while sui is linked)
#   zfsapp                 reading and writing files
#   zcfg                   apps.arcade.skill, which the runtime reads
#   zarcade, zarcade_sys   the arcade runtime: window or full screen
#   zsfx                   linked by the runtime; pix makes no sound
#   zsprite                sprite sheets and their tiered palette (zsprite.h)
#   zimg, zinflate         pictures from elsewhere, in colour (px_import.c):
#                          PNG on, and the colour rows (zimg.h)
OBJS = zeitlos.o zobj.o zgfx.o zcolor.o zfont_data.o zwin.o zwidget.o \
	zspeak.o zdialog.o zflist.o zedit.o zfsapp.o \
	zarcade.o zarcade_sys.o zsfx.o zcfg.o sui.o \
	zsprite.o zimg.o zinflate.o \
	px_doc.o px_io.o px_edit.o px_anim.o px_import.o pix.o

APP_CFLAGS = -DZ_IMG_HAVE_PNG=1 -DZ_IMG_HAVE_RGB=1

test:
	$(MAKE) -C tests

clean: clean-tests
clean-tests:
	$(MAKE) -C tests clean

.PHONY: test clean-tests

include ../../common/app.mk
