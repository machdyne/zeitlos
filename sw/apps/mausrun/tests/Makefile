# sw/apps/mausrun/tests -- host tools and tests.
#
#   make            the level parser and the progress file (ASan, UBSan),
#                   then levelcheck over every level: each one must be
#                   possible to finish, with every cheese in reach
#   make render     every level as one long picture, out/level_NN.ppm
#                   (and .png where Python has PIL)

HOSTCC ?= cc
SAN = -fsanitize=address,undefined -fno-sanitize-recover=undefined
# no ASan for the game render: its shadow memory covers the register
# addresses mr_host.c maps (0xc0000000)
UBSAN = -fsanitize=undefined -fno-sanitize-recover=undefined
HOSTFLAGS = -std=gnu99 -Wall -Wextra -O1 -g $(SAN)

LEVELS = $(sort $(wildcard ../../../data/mausrun/levels/*.txt))
CORE = ../level.c ../levels_builtin.c ../sprites.c

all: test

test: test_level test_sound test_art levelcheck sheets
	./test_level
	./test_art
	./test_sound
	./levelcheck $(LEVELS)
	@# and it must say no to a level that cannot be finished
	@if ./levelcheck unfinishable.txt >/dev/null; then \
		echo "levelcheck passed unfinishable.txt"; exit 1; \
	else echo "levelcheck: refuses unfinishable.txt, as it should"; fi

test_level: test_level.c ../level.c ../progress.c ../level.h ../progress.h
	$(HOSTCC) $(HOSTFLAGS) -o $@ test_level.c ../level.c ../levels_builtin.c ../progress.c ../sprites.c

# the card's sheets over the art (../art.c, sw/common/zsprite.c)
test_art: test_art.c ../art.c ../art.h ../sprites.c ../../../common/zsprite.c
	$(HOSTCC) $(HOSTFLAGS) -DART_HOST -o $@ test_art.c ../art.c ../sprites.c ../../../common/zsprite.c

# sprites.c and sprites.h are what gen_sprites.py makes of the sheets
# in sw/data/mausrun: run it after editing a sheet, and commit the two
sheets:
	mkdir -p gen
	python3 ../gen_sprites.py --out gen
	cmp gen/sprites.c ../sprites.c && cmp gen/sprites.h ../sprites.h
	@echo "sheets: sprites.c and sprites.h are up to date with the sheets"

test_sound: test_sound.c ../sound.c ../sound.h ../../../common/games/zsfx.c
	$(HOSTCC) $(HOSTFLAGS) -DZSFX_HOSTED -o $@ test_sound.c ../sound.c ../../../common/games/zsfx.c

# optimised: it searches every way through a level
levelcheck: levelcheck.c ../phys.c $(CORE) ../phys.h ../level.h
	$(HOSTCC) -std=gnu99 -Wall -Wextra -O2 -o $@ levelcheck.c ../phys.c $(CORE)

levelstrip: levelstrip.c $(CORE) ../level.h
	$(HOSTCC) $(HOSTFLAGS) -o $@ levelstrip.c $(CORE)

# the game itself on a software machine (host/): every screen
GAME_SRC = host/render.c host/mr_host.c host/mr_wrap_game.c host/mr_wrap_lib.c \
	../art.c ../../../common/zsprite.c ../level.c ../levels_builtin.c ../phys.c ../progress.c ../sprites.c ../sound.c ../../../common/zfont_data.c
render_mausrun: $(GAME_SRC) ../mausrun.c host/mr_host.h
	$(HOSTCC) -std=gnu99 -O1 -g $(UBSAN) -Wall -Wno-unused-function -DMAUSRUN_PROF \
		-DZ_APP_NAME='"mausrun"' -o $@ $(GAME_SRC)

render: levelstrip render_mausrun
	mkdir -p out
	./levelstrip out $(LEVELS)
	./render_mausrun out
	./render_mausrun out garden
	./render_mausrun out themes
	-python3 ../../snake/tests/ppm2png.py out

check: levelcheck
	./levelcheck -v $(LEVELS)

clean:
	rm -rf test_level test_sound test_art levelcheck levelstrip render_mausrun out gen

.PHONY: all test render check clean
