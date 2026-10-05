# zfpga -- device build: zfpga.bin, an ordinary Zeitlos app.
#
#   make                 zfpga.bin and db/lfe5u25f.zdb
#   make -f Makefile.host && ./tests/run.sh    host build and tests
#
# Same sources as Makefile.host; only the port file differs. The shape
# is sw/apps/zcc/Makefile's, for the reasons recorded there.

# Build rules are shared: sw/common/app.mk, docs/build.md.

APP = zfpga
APP_MAIN_RULE = 0

# -DZFPGA_HOSTED=0 on EVERYTHING: main.c tests it too, for the argv
# handling, and a device build that trusted main()'s argc/argv is the
# bug that crashed a machine under zcc (docs/zcc.md).
APP_CFLAGS = -I../../common -I. -DZFPGA_HOSTED=0

# -Werror on zfpga's own sources only; sw/common's warnings vary by
# toolchain version and are not this app's to fail over.
OWN_CFLAGS = $(CFLAGS) -Werror -Wno-unused-parameter

# Objects in build/, never beside the sources, where the host build
# would overwrite them (sw/apps/zcc/Makefile has the story).
B = build
COMMON = zeitlos zobj zport zfsapp zwin zgfx zfont_data
OWN = main util db chip cfg pack pnr route place unpack synth_parse synth_flat synth_elab synth_map bram build boot port_dev
OBJS = $(addprefix $(B)/,$(addsuffix .o,$(COMMON) $(OWN)))

DB_SRC = ext/prjtrellis-db
DB_COMMIT = 015e0330630d7c238c0e4f2cdd9c8157eb78c54a

zfpga: db/lfe5u25f.zdb card

$(addprefix $(B)/,$(addsuffix .o,$(COMMON))): $(B)/%.o: ../../common/%.c | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

$(addprefix $(B)/,$(addsuffix .o,$(OWN))): $(B)/%.o: %.c zfpga.h zfpga_port.h synth.h | $(B)
	$(CC) $(OWN_CFLAGS) -c $< -o $@

$(B):
	mkdir -p $(B)

# The database the card carries as /fpga/lfe5u25f.zdb. It serves the
# 12F too (identical die). Rebuilt when the generator changes; the
# vendored data changes only with a deliberate refresh.
# db/ is the card's /fpga exactly: the database, the board profiles with
# their pins, and the examples (docs/zfpga-formats.md sec. 7).
CARD_EXAMPLES = examples/blink.v examples/blinkf.v examples/empty.zn examples/on.zn \
	examples/blink.zn examples/hand.zl examples/blinkh.v examples/blinkh.vh

card: db/lfe5u25f.zdb db/lfe5u45f.zdb boards/lakritz.brd ../../../boards/lakritz_v0.lpf boards/obst.brd ../../../boards/obst_v0.lpf boards/mozart1.brd ../../../boards/mozart_ml1.lpf boards/sergei1.brd ../../../boards/sergei_ml1.lpf boards/mozart2.brd ../../../boards/mozart_ml2.lpf boards/sergei2.brd ../../../boards/sergei_ml2.lpf boards/konfekt.brd ../../../boards/konfekt_v0.lpf boards/minze.brd ../../../boards/minze_v1.lpf boards/schoko.brd ../../../boards/schoko_v1.lpf boards/noir.brd ../../../boards/noir_v0.lpf boards/klinge.brd ../../../boards/klinge_v1.lpf $(CARD_EXAMPLES)
	mkdir -p db/boards db/examples
	cp boards/lakritz.brd db/boards/lakritz.brd
	cp ../../../boards/lakritz_v0.lpf db/boards/lakritz.lpf
	cp boards/obst.brd db/boards/obst.brd
	cp ../../../boards/obst_v0.lpf db/boards/obst.lpf
	cp boards/mozart1.brd db/boards/mozart1.brd
	cp ../../../boards/mozart_ml1.lpf db/boards/mozart1.lpf
	cp boards/sergei1.brd db/boards/sergei1.brd
	cp ../../../boards/sergei_ml1.lpf db/boards/sergei1.lpf
	cp boards/mozart2.brd db/boards/mozart2.brd
	cp ../../../boards/mozart_ml2.lpf db/boards/mozart2.lpf
	cp boards/sergei2.brd db/boards/sergei2.brd
	cp ../../../boards/sergei_ml2.lpf db/boards/sergei2.lpf
	cp boards/konfekt.brd db/boards/konfekt.brd
	cp ../../../boards/konfekt_v0.lpf db/boards/konfekt.lpf
	cp boards/minze.brd db/boards/minze.brd
	cp ../../../boards/minze_v1.lpf db/boards/minze.lpf
	cp boards/schoko.brd db/boards/schoko.brd
	cp ../../../boards/schoko_v1.lpf db/boards/schoko.lpf
	cp boards/noir.brd db/boards/noir.brd
	cp ../../../boards/noir_v0.lpf db/boards/noir.lpf
	cp boards/klinge.brd db/boards/klinge.brd
	cp ../../../boards/klinge_v1.lpf db/boards/klinge.lpf
	cp $(CARD_EXAMPLES) db/examples/

db/lfe5u25f.zdb: tools/mkzdb.py ext/nextpnr-base/lfe5u-25f.config
	mkdir -p db
	python3 tools/mkzdb.py --db $(DB_SRC) --device LFE5U-25F --also LFE5U-12F \
		--source $(DB_COMMIT) --base ext/nextpnr-base/lfe5u-25f.config -o $@

# The 45F: Mozart ML1 and Sergei ML1 (docs/zboot.md sec. 5)
db/lfe5u45f.zdb: tools/mkzdb.py ext/nextpnr-base/lfe5u-45f.config
	mkdir -p db
	python3 tools/mkzdb.py --db $(DB_SRC) --device LFE5U-45F \
		--source $(DB_COMMIT) --base ext/nextpnr-base/lfe5u-45f.config -o $@

.PHONY: card

CLEAN_EXTRA = $(B) db tests/out

include ../../common/app.mk
