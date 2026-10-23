# sw/common/games/ui -- the screen toolkit (sui.h, docs/screen_ui.md).
#
# No target build: apps compile sui.c in, as they do zarcade.c (add
# $(COMMON_DIR)/games/ui to SRC_DIRS and sui.o to OBJS).
#
#   make test     every box in every mode on the host's software
#                 framebuffer (tests/test_sui.c), each frame checked
#                 against a full repaint, with ASan and UBSan
#   make render   the same, and the pictures as PNG (needs PIL) in
#                 tests/out/<mode>/

HOSTCC ?= cc
SAN = -fsanitize=address,undefined -fno-sanitize-recover=undefined
HOSTFLAGS = -std=gnu99 -Wall -Wextra -O1 -g $(SAN)

GAMES = ..
SRC = sui.c $(GAMES)/zarcade.c $(GAMES)/zsfx.c $(GAMES)/tests/ar_host.c \
	../../zfont_data.c

MODES = window mono c16 c16auto c64 c256

tests/test_sui: tests/test_sui.c $(SRC) sui.h $(GAMES)/zarcade.h
	$(HOSTCC) $(HOSTFLAGS) -DZSFX_HOSTED -DAR_HOST_TESTS -o $@ \
		tests/test_sui.c $(SRC)

# a card for the file boxes: /home with folders, a picture folder and
# enough files to scroll
FS = tests/out/fs
fs:
	@rm -rf $(FS) && mkdir -p $(FS)/home/fonts $(FS)/home/pictures $(FS)/docs
	@touch $(FS)/home/pictures/cat.zbm $(FS)/home/pictures/dog.zbm $(FS)/home/readme.txt
	@for i in 01 02 03 04 05 06 07 08 09 10 11 12 13 14 15 16 17 18 19 20 \
		21 22 23 24 25 26 27 28 29 30; do touch $(FS)/home/file$$i.zbm; done

test: tests/test_sui fs
	@for m in $(MODES); do \
		mkdir -p tests/out/$$m; \
		AR_HOST_FS=$(FS) AR_HOST_VERIFY=1 ./tests/test_sui tests/out/$$m $$m > tests/out/$$m.log || \
			{ cat tests/out/$$m.log; exit 1; }; \
		tail -1 tests/out/$$m.log; \
	done
	@mkdir -p tests/out/p6
	@AR_HOST_FS=$(FS) AR_HOST_VERIFY=1 AR_HOST_PLANES=6 ./tests/test_sui tests/out/p6 c256 > tests/out/p6.log || \
		{ cat tests/out/p6.log; exit 1; }
	@tail -1 tests/out/p6.log

render: test
	@for m in $(MODES) p6; do \
		python3 ../../../apps/pix/tests/ppm2png.py tests/out/$$m > /dev/null; \
	done
	@echo "render: tests/out/<mode>/*.png, and sheet.png in each"

includecheck:
	@$(HOSTCC) -std=gnu99 -Wall -Wextra -fsyntax-only sui.c
	@echo "includecheck: sui.c resolves its headers with no -I"

clean:
	rm -rf tests/test_sui tests/out

.PHONY: test fs render includecheck clean
