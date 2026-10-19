# sw/apps/snake/tests -- host tests and pictures.
#
#   make            the rules (sn_game.c), with ASan and UBSan
#   make render     the real game loop on a software framebuffer
#                   (sw/common/games/tests/ar_host.c), driven by a
#                   script; every screen written to out/ as PPM, and as
#                   PNG when Python's PIL is there

HOSTCC ?= cc
SAN = -fsanitize=address,undefined -fno-sanitize-recover=undefined
HOSTFLAGS = -std=gnu99 -Wall -Wextra -O1 -g $(SAN)

COMMON = ../../../common
GAMES = $(COMMON)/games

RULES = ../sn_game.c ../sn_levels.c

all: test

test: test_rules
	./test_rules

test_rules: test_rules.c $(RULES) ../sn_game.h
	$(HOSTCC) $(HOSTFLAGS) -o $@ test_rules.c $(RULES)

GAME_SRC = ../snake.c ../sn_draw.c ../sn_sound.c $(RULES)
RT_SRC = $(GAMES)/zarcade.c $(GAMES)/zsfx.c $(GAMES)/tests/ar_host.c \
	$(COMMON)/zfont_data.c

render_snake: render.c $(GAME_SRC) $(RT_SRC)
	$(HOSTCC) $(HOSTFLAGS) -DZSFX_HOSTED -DAR_HOST_TESTS -DSN_AUTOPLAY -Dmain=game_main -Wno-unused-parameter \
		-o $@ render.c $(GAME_SRC) $(RT_SRC)

render: render_snake
	mkdir -p out
	AR_HOST_VERIFY=1 ./render_snake out
	-python3 ppm2png.py out

clean:
	rm -rf test_rules render_snake out

.PHONY: all test render clean
