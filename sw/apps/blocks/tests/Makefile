# sw/apps/blocks/tests -- host tests and pictures.
#
#   make            the rules (bk_game.c), with ASan and UBSan
#   make render     the real game loop on a software framebuffer
#                   (sw/common/games/tests/ar_host.c), driven by a
#                   script, the autopilot playing; every screen to out/

HOSTCC ?= cc
SAN = -fsanitize=address,undefined -fno-sanitize-recover=undefined
HOSTFLAGS = -std=gnu99 -Wall -Wextra -O1 -g $(SAN)

COMMON = ../../../common
GAMES = $(COMMON)/games

RULES = ../bk_game.c

all: test

test: test_rules
	./test_rules

test_rules: test_rules.c $(RULES) ../bk_game.h
	$(HOSTCC) $(HOSTFLAGS) -DBK_TESTS -o $@ test_rules.c $(RULES)

GAME_SRC = ../blocks.c ../bk_draw.c ../bk_sound.c $(RULES)
RT_SRC = $(GAMES)/zarcade.c $(GAMES)/zsfx.c $(GAMES)/tests/ar_host.c \
	$(COMMON)/zfont_data.c

render_blocks: render.c $(GAME_SRC) $(RT_SRC) ../*.h
	$(HOSTCC) $(HOSTFLAGS) -DZSFX_HOSTED -DAR_HOST_TESTS -DBK_AUTOPLAY -Dmain=game_main \
		-Wno-unused-parameter -o $@ render.c $(GAME_SRC) $(RT_SRC)

render: render_blocks
	mkdir -p out
	AR_HOST_VERIFY=1 ./render_blocks out
	-python3 ../../snake/tests/ppm2png.py out

clean:
	rm -rf test_rules render_blocks out

.PHONY: all test render clean
