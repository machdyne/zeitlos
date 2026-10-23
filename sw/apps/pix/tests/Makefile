# sw/apps/pix/tests -- host tests.
#
#   make render     the editor itself on a software framebuffer, every
#                   screen in out/ (PPM, and PNG with PIL)
#   make            the document, the file formats and the tools
#                   (px_doc.c, px_io.c, px_edit.c), with ASan and UBSan

HOSTCC ?= cc
SAN = -fsanitize=address,undefined -fno-sanitize-recover=undefined
HOSTFLAGS = -std=gnu99 -Wall -Wextra -O1 -g $(SAN)

all: test

test: test_io test_edit test_anim test_import
	./test_io
	./test_edit
	./test_anim
	python3 gen_import.py out/import
	./test_import out/import

ZSP = ../../../common/zsprite.c

test_io: test_io.c ../px_doc.c ../px_io.c ../px_doc.h ../px_io.h $(ZSP)
	$(HOSTCC) $(HOSTFLAGS) -o $@ test_io.c ../px_doc.c ../px_io.c $(ZSP)

test_edit: test_edit.c ../px_doc.c ../px_edit.c ../px_doc.h ../px_edit.h $(ZSP)
	$(HOSTCC) $(HOSTFLAGS) -o $@ test_edit.c ../px_doc.c ../px_edit.c $(ZSP)

test_anim: test_anim.c ../px_doc.c ../px_io.c ../px_anim.c ../px_doc.h ../px_io.h ../px_anim.h $(ZSP)
	$(HOSTCC) $(HOSTFLAGS) -o $@ test_anim.c ../px_doc.c ../px_io.c ../px_anim.c $(ZSP)

IMG = ../../../common/zimg.c ../../../common/zinflate.c
IMG_DEFS = -DZ_IMG_HAVE_PNG=1 -DZ_IMG_HAVE_RGB=1 -I../../../common

test_import: test_import.c ../px_doc.c ../px_io.c ../px_edit.c ../px_import.c ../px_import.h $(ZSP) $(IMG)
	$(HOSTCC) $(HOSTFLAGS) $(IMG_DEFS) -o $@ test_import.c ../px_doc.c ../px_io.c ../px_edit.c ../px_import.c $(ZSP) $(IMG)

# the editor on a software framebuffer, driven by a script (render.c)
COMMON = ../../../common
GAMES = $(COMMON)/games
RT_SRC = $(GAMES)/zarcade.c $(GAMES)/zsfx.c $(GAMES)/tests/ar_host.c \
	$(GAMES)/ui/sui.c $(COMMON)/zfont_data.c
PIX_SRC = ../pix.c ../px_doc.c ../px_io.c ../px_edit.c ../px_anim.c ../px_import.c $(ZSP) $(IMG)

render_pix: render.c $(PIX_SRC) $(RT_SRC)
	$(HOSTCC) $(HOSTFLAGS) $(IMG_DEFS) -DZSFX_HOSTED -DAR_HOST_TESTS -Dmain=game_main -Wno-unused-parameter \
		-o $@ render.c $(PIX_SRC) $(RT_SRC)

# AR_HOST_VERIFY: every frame checked against a full repaint
render: render_pix
	mkdir -p out out/p6 out/p4
	AR_HOST_VERIFY=1 ./render_pix out icons
	AR_HOST_VERIFY=1 ./render_pix out font
	AR_HOST_VERIFY=1 ./render_pix out cursor
	AR_HOST_VERIFY=1 ./render_pix out picture
	AR_HOST_VERIFY=1 ./render_pix out colour
	AR_HOST_VERIFY=1 ./render_pix out c256
	AR_HOST_VERIFY=1 ./render_pix out mausrun
	AR_HOST_VERIFY=1 ./render_pix out anim
	AR_HOST_VERIFY=1 ./render_pix out cpic
	python3 gen_import.py out/import
	AR_HOST_VERIFY=1 ./render_pix out import
	AR_HOST_VERIFY=1 AR_HOST_PLANES=6 ./render_pix out/p6 c256
	AR_HOST_VERIFY=1 AR_HOST_PLANES=4 ./render_pix out/p4 c256
	mkdir -p out/boxes out/fs/home/pictures
	touch out/fs/home/pictures/cat.zbm out/fs/home/walk.zsp out/fs/home/icons.zic
	AR_HOST_VERIFY=1 AR_HOST_FS=out/fs ./render_pix out/boxes boxes
	-python3 ppm2png.py out/boxes
	mkdir -p out/wc8 out/wc6
	AR_HOST_VERIFY=1 ./render_pix out/wc8 wcolour
	AR_HOST_VERIFY=1 ./render_pix out/wc8 wc256
	AR_HOST_VERIFY=1 AR_HOST_PLANES=6 ./render_pix out/wc6 wcolour
	AR_HOST_VERIFY=1 AR_HOST_PLANES=6 ./render_pix out/wc6 wc256
	-python3 ppm2png.py out/wc8
	-python3 ppm2png.py out/wc6
	-python3 ppm2png.py out

clean:
	rm -rf test_io test_edit test_anim test_import render_pix out

.PHONY: all test clean
