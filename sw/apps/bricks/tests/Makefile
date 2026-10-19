# sw/apps/bricks/tests -- host tests and pictures.
#
#   make            the rules (br_game.c), with ASan and UBSan
#   make render     the real game loop on a software framebuffer
#                   (sw/common/games/tests/ar_host.c), driven by a
#                   script, the autopilot playing; every screen to out/

HOSTCC ?= cc
SAN = -fsanitize=address,undefined -fno-sanitize-recover=undefined
HOSTFLAGS = -std=gnu99 -Wall -Wextra -O1 -g $(SAN)

COMMON = ../../../common
GAMES = $(COMMON)/games

RULES = ../br_game.c

all: test

test: test_rules
	./test_rules

test_rules: test_rules.c $(RULES) ../br_game.h
	$(HOSTCC) $(HOSTFLAGS) -DBR_TESTS -o $@ test_rules.c $(RULES)

GAME_SRC = ../bricks.c ../br_draw.c ../br_sound.c $(RULES)
RT_SRC = $(GAMES)/zarcade.c $(GAMES)/zsfx.c $(GAMES)/tests/ar_host.c \
	$(COMMON)/zfont_data.c

render_bricks: render.c $(GAME_SRC) $(RT_SRC) ../*.h
	$(HOSTCC) $(HOSTFLAGS) -DZSFX_HOSTED -DAR_HOST_TESTS -DBR_AUTOPLAY -Dmain=game_main \
		-Wno-unused-parameter -o $@ render.c $(GAME_SRC) $(RT_SRC)

render: render_bricks
	mkdir -p out
	AR_HOST_VERIFY=1 ./render_bricks out
	-python3 ../../snake/tests/ppm2png.py out

clean:
	rm -rf test_rules render_bricks out

.PHONY: all test render clean
