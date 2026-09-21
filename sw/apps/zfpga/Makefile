# zfpga -- device build: zfpga.bin, an ordinary Zeitlos app.
#
#   make                 zfpga.bin and db/lfe5u25f.zdb
#   make -f Makefile.host && ./tests/run.sh    host build and tests
#
# Same sources as Makefile.host; only the port file differs. The shape
# is sw/apps/zcc/Makefile's, for the reasons recorded there.

PREFIX ?= $(RISCV_PREFIX)
include ../../common/arch.mk
CC = $(PREFIX)gcc

GC_SECTIONS ?= 1
ifeq ($(GC_SECTIONS),1)
GC_CFLAGS = -ffunction-sections -fdata-sections
GC_LDFLAGS = -Wl,--gc-sections
endif

# -DZFPGA_HOSTED=0 on EVERYTHING: main.c tests it too, for the argv
# handling, and a device build that trusted main()'s argc/argv is the
# bug that crashed a machine under zcc (docs/zcc.md).
override CFLAGS += --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=$(ABI) \
	$(GC_CFLAGS) $(ARCH_DEFS) $(LIBC_FLAGS) -I../../common -I. \
	-DZFPGA_HOSTED=0
LDFLAGS = -march=$(ARCH) -mabi=$(ABI) $(GC_LDFLAGS) $(LIBC_FLAGS)
LDSCRIPT = ../../common/riscv-app.ld

# -Werror on zfpga's own sources only; sw/common's warnings vary by
# toolchain version and are not this app's to fail over.
APP_CFLAGS = $(CFLAGS) -Werror -Wno-unused-parameter

# Objects in build/, never beside the sources, where the host build
# would overwrite them (sw/apps/zcc/Makefile has the story).
B = build
COMMON = zeitlos zobj zport zfsapp zwin zgfx zfont_data
APP = main util db chip cfg pack pnr route place unpack synth_parse synth_flat synth_elab synth_map bram build port_dev
OBJS = $(addprefix $(B)/,$(addsuffix .o,$(COMMON) $(APP)))

DB_SRC = ext/prjtrellis-db
DB_COMMIT = 015e0330630d7c238c0e4f2cdd9c8157eb78c54a

zfpga: zfpga.bin db/lfe5u25f.zdb card

$(addprefix $(B)/,$(addsuffix .o,$(COMMON))): $(B)/%.o: | $(B)
	$(CC) $(CFLAGS) -c ../../common/$*.c -o $@

$(addprefix $(B)/,$(addsuffix .o,$(APP))): $(B)/%.o: %.c zfpga.h zfpga_port.h synth.h | $(B)
	$(CC) $(APP_CFLAGS) -c $< -o $@

$(B):
	mkdir -p $(B)

zfpga.elf: $(OBJS)
	$(CC) $(LDFLAGS) -T $(LDSCRIPT) -Wl,-Map,zfpga.map -o zfpga.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble zfpga.elf > zfpga.dasm

zfpga.bin: zfpga.elf
	@EDATA=$$($(PREFIX)nm zfpga.elf | awk '$$3=="_edata"{print "0x"$$1}'); \
	END=$$($(PREFIX)nm zfpga.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	$(PREFIX)objcopy -O binary zfpga.elf zfpga.data; \
	python3 ../../../tools/mkexec.py zfpga.data zfpga.bin $$(($$END - $$EDATA)); \
	rm -f zfpga.data

# The database the card carries as /fpga/lfe5u25f.zdb. It serves the
# 12F too (identical die). Rebuilt when the generator changes; the
# vendored data changes only with a deliberate refresh.
# db/ is the card's /fpga exactly: the database, the board profiles with
# their pins, and the examples (docs/zfpga-formats.md sec. 7).
CARD_EXAMPLES = examples/blink.v examples/blinkf.v examples/empty.zn examples/on.zn \
	examples/blink.zn examples/hand.zl examples/blinkh.v examples/blinkh.vh

card: db/lfe5u25f.zdb boards/lakritz.brd ../../../boards/lakritz_v0.lpf $(CARD_EXAMPLES)
	mkdir -p db/boards db/examples
	cp boards/lakritz.brd db/boards/lakritz.brd
	cp ../../../boards/lakritz_v0.lpf db/boards/lakritz.lpf
	cp $(CARD_EXAMPLES) db/examples/

db/lfe5u25f.zdb: tools/mkzdb.py ext/nextpnr-base/lfe5u-25f.config
	mkdir -p db
	python3 tools/mkzdb.py --db $(DB_SRC) --device LFE5U-25F --also LFE5U-12F \
		--source $(DB_COMMIT) --base ext/nextpnr-base/lfe5u-25f.config -o $@

clean:
	rm -rf $(B) zfpga.elf zfpga.bin zfpga.map zfpga.dasm db tests/out

.PHONY: zfpga clean card

-include $(wildcard $(B)/*.d)
