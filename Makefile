RTL_PICO = \
	rtl/sysctl.v \
	rtl/clk/pll0.v \
	rtl/clk/pll1.v \
	rtl/clk/pll0_25.v \
	rtl/clk/pll1_25.v \
	rtl/cpu/picorv32/picorv32.v \
	rtl/cpu/zeitlos32/zeitlos32.v \
	rtl/cpu/zeitlos32/zeitlos32_muldiv.v \
	rtl/mtu.v \
	rtl/cache.v \
	rtl/arbiter_vram.v \
	rtl/arbiter_main.v \
	rtl/mem/bram.v \
	rtl/mem/sram.v \
	rtl/mem/sdram_kianv.v \
	rtl/mem/qqspi.v \
	rtl/mem/vram.v \
	rtl/mem/glyph.v \
	rtl/spiflashro.v \
	rtl/uart_null.v \
	rtl/ethmac_rmii.v \
	rtl/gpio.v \
	rtl/csrs.v \
	rtl/esp32_rxfifo.v \
	rtl/socctl.v \
	rtl/rtc.v \
	rtl/trng.v \
	rtl/montmul.v \
	rtl/audio.v \
	rtl/audio_out.v \
	rtl/audio_mixer.v \
	rtl/audio_spdif.v \
	rtl/spim.v \
	rtl/gpu/gpu_raster.v \
	rtl/gpu/gpu_blit.v \
	rtl/gpu/gpu_video.v \
	rtl/gpu/gpu_cursor.v \
	rtl/gpu/gpu_ddmi.v \
	rtl/gpu/tmds_encoder.v \
	rtl/uart.v \
	rtl/usb_hid.v \
	rtl/usb/usb_sie.v \
	rtl/usb/usb_port.v \
	rtl/usb/usb_xact.v \
	rtl/usb/usb_hid_compat.v \
	rtl/usb/usb_host.v \
	rtl/probe.v \
	rtl/ext/usb_hid_host/src/usb_hid_host.v \
	rtl/ext/usb_hid_host/src/usb_hid_host_rom.v \
	rtl/usb_cdc_uart.v \
	rtl/ext/usb_cdc/usb_cdc.v \
	rtl/ext/usb_cdc/sie.v \
	rtl/ext/usb_cdc/phy_rx.v \
	rtl/ext/usb_cdc/phy_tx.v \
	rtl/ext/usb_cdc/ctrl_endp.v \
	rtl/ext/usb_cdc/bulk_endp.v \
	rtl/ext/usb_cdc/in_fifo.v \
	rtl/ext/usb_cdc/out_fifo.v

ifndef CABLE
	CABLE = dirtyJtag
endif

# RISC-V prefix forwarded to sw/bios and sw/os. Override for a
# multi-lib gcc, e.g. PREFIX=/opt/riscv/bin/riscv64-unknown-elf-
PREFIX ?= /opt/riscv32i/bin/riscv32-unknown-elf-

# Extra defines handed to the synthesis tool, on top of the
# -DBOARD_<X> and -D<FAMILY> the recipes below always pass.
#
# Empty for every ordinary build. It exists for release/zrelease, which
# passes -DZSPEC to make rtl/boards.vh use a generated define set
# instead of its own per-board `ifdef chain -- see that file's ZSPEC
# note for why a release needs to be able to build a board WITHOUT a
# feature, which a command-line -D cannot express.
EXTRA_DEFINES ?=

# ECP5 LUT mapper. `-abc9` is yosys's timing-driven mapper, which uses
# real ECP5 cell delays rather than counting LUT levels. It is the
# default because the legacy mapper (plain `abc -lut 4:7`) is
# depth-first: it finds the shallowest achievable mapping for the whole
# design and then recovers area under that ceiling, so whenever a
# change removes the deepest path it responds by spending LUT6/LUT7s
# on everything that just became near-critical. On this SoC that
# turned a 40-line blitter change into +1500 LUT4s; abc9 mapped the
# same two netlists to within six LUTs of each other.
#
# Costs a slower yosys step (roughly 1.5-2x). Correctness is not in
# question either way -- both mappers are equivalence-preserving.
#
# Override on the command line to compare:  make BOARD=obst ABC9=
ABC9 ?= -abc9

main: check zeitlos

check:
ifndef BOARD
	@echo must set BOARD variable \(make BOARD=obst\)
	@exit 1
endif

BOARD_LC = $(shell echo '$(BOARD)' | tr '[:upper:]' '[:lower:]')
BOARD_UC = $(shell echo '$(BOARD)' | tr '[:lower:]' '[:upper:]')

# Where the bitstream and everything alongside it lands.
#
# Overridable so that a build which is NOT your development build can
# be kept out of the way of one that is. release/zrelease sets
#
#   OUTDIR=output/releases/<board>
#
# and wipes that directory before each target, because two targets on
# one board (lakritz_gpio and lakritz_langkatze) would otherwise share
# a directory and a failed second build would leave the first one's
# soc.bit sitting there looking valid. Without this override that wipe
# would take your working bitstream with it.
#
# Anything using it must not assume the directory exists -- every
# recipe below mkdir -p's it.
OUTDIR ?= output/$(BOARD_LC)

ifeq ($(BOARD_LC), riegel)
	FAMILY = ice40
	DEVICE = hx4k
	PACKAGE = bg121
	PCF = riegel.pcf
	PROG = ldprog -s
	FLASH = ldprog -f
else ifeq ($(BOARD), eis)
	FAMILY = ice40
	DEVICE = hx4k
	PACKAGE = bg121
	PCF = eis.pcf
	PROG = ldprog -is
	FLASH = ldprog -if
else ifeq ($(BOARD), kolibri)
	FAMILY = ice40
	DEVICE = hx4k
	PACKAGE = bg121
	PCF = kolibri.pcf
	PROG = ldprog -Ks
	FLASH = ldprog -Kf
else ifeq ($(BOARD), bonbon)
	FAMILY = ice40
	DEVICE = up5k
	PACKAGE = sg48
	PCF = bonbon.pcf
	PROG = ldprog -bs
	FLASH = ldprog -bf
else ifeq ($(BOARD), keks)
	FAMILY = ice40
	DEVICE = hx8k
	PACKAGE = ct256
	PCF = keks.pcf
	PROG = ldprog -ks
	FLASH = ldprog -kf
else ifeq ($(BOARD), kuchen_v0)
	FAMILY = ice40
	DEVICE = hx8k
	PACKAGE = ct256
	PCF = kuchen_v0.pcf
	PROG = ldprog -s
	FLASH = ldprog -f
else ifeq ($(BOARD), kuchen)
	FAMILY = ice40
	DEVICE = hx8k
	PACKAGE = ct256
	PCF = kuchen_v1.pcf
	PROG = ldprog -s
	FLASH = ldprog -f
else ifeq ($(BOARD), brot)
	FAMILY = ice40
	DEVICE = up5k
	PACKAGE = sg48
	PCF = brot_v4.pcf
	PROG = ldprog -s
	FLASH = ldprog -f
else ifeq ($(BOARD), krote)
	FAMILY = ice40
	DEVICE = hx4k
	PACKAGE = bg121
	PCF = krote.pcf
	PROG = ldprog -s
	FLASH = ldprog -f
else ifeq ($(BOARD), icoboard)
	FAMILY = ice40
	DEVICE = hx8k
	PACKAGE = ct256
	PCF = icoboard.pcf
	PROG = icoprog -p < output/soc.bit
	FLASH = icoprog -f < output/soc.bit
	FLASH_OFFSET = -O
else ifeq ($(BOARD), schoko)
	FAMILY = ecp5
	DEVICE = 45k
	PACKAGE = CABGA256
	LPF = schoko_v1.lpf
	PROG = openFPGALoader -c $(CABLE)
	FLASH = openFPGALoader -c $(CABLE) -f
	FLASH_OFFSET = -o
else ifeq ($(BOARD), konfekt)
	FAMILY = ecp5
	DEVICE = 12k
	PACKAGE = CABGA256
	LPF = konfekt_v0.lpf
	PROG = openFPGALoader -c $(CABLE)
	FLASH = openFPGALoader -v -c $(CABLE) -f
	FLASH_OFFSET = -o
else ifeq ($(BOARD), minze)
	FAMILY = ecp5
	DEVICE = 12k
	PACKAGE = CABGA256
	LPF = minze_v1.lpf
	PROG = openFPGALoader -c $(CABLE)
	FLASH = openFPGALoader -v -c $(CABLE) -f
	FLASH_OFFSET = -o
else ifeq ($(BOARD), vanille)
	FAMILY = ecp5
	DEVICE = 12k
	PACKAGE = TQFP144
	LPF = vanille_v2.lpf
	PROG = openFPGALoader -c $(CABLE)
	FLASH = openFPGALoader -v -c $(CABLE) -f
	FLASH_OFFSET = -o
else ifeq ($(BOARD), obst)
	FAMILY = ecp5
	DEVICE = 12k
	PACKAGE = CABGA256
	LPF = obst_v0.lpf
	PROG = openFPGALoader -c $(CABLE)
	FLASH = openFPGALoader -v -c $(CABLE) -f
	FLASH_OFFSET = -o
else ifeq ($(BOARD), lakritz)
	FAMILY = ecp5
	DEVICE = 25k
	PACKAGE = CABGA256
	LPF = lakritz_v0.lpf
	PROG = openFPGALoader -c $(CABLE)
	FLASH = openFPGALoader -v -c $(CABLE) -f
	FLASH_OFFSET = -o
else ifeq ($(BOARD), mozart_ml1)
	FAMILY = ecp5
	DEVICE = 45k
	PACKAGE = CABGA256
	LPF = mozart_ml1.lpf
	PROG = openFPGALoader -c dirtyJtag
	FLASH = openFPGALoader -v -c dirtyJtag -f
	FLASH_OFFSET = -o
else ifeq ($(BOARD), sergei_ml1)
	FAMILY = ecp5
	DEVICE = 45k
	PACKAGE = CABGA256
	LPF = sergei_ml1.lpf
	PROG = openFPGALoader -c dirtyJtag
	FLASH = openFPGALoader -v -c dirtyJtag -f
	FLASH_OFFSET = -o
else ifeq ($(BOARD), ulx3s)
	FAMILY = ecp5
	# Same 12/25/45/85K PCB and LPF; DEVICE picks the fitted chip.
	# Default matches upstream (25k) -- pass DEVICE=85k on an 85F board.
	DEVICE ?= 25k
	PACKAGE = CABGA381
	LPF = ulx3s.lpf
	PROG = openFPGALoader -b ulx3s
	FLASH = openFPGALoader -v -b ulx3s -f
		# Placement seed. This design sits close to 48MHz on the 85F, and
	# which seed meets it is luck: the spread across seeds is roughly
	# 45-50MHz, and nextpnr's default is among the ones that miss. A
	# bitstream that misses timing programs fine and then misbehaves
	# intermittently, so a seed known to meet it is pinned here.
	# Re-check after any RTL or pin change (`make ... timing`), and
	# override with PNR_SEED= on the command line.
	PNR_SEED ?= 10
FLASH_OFFSET = -o
else ifeq ($(BOARD), lebkuchen)
	FAMILY = gatemate
	DEVICE = ccgma1
	CABLE = dirtyJtag
	CCF = boards/lebkuchen_v0.ccf
	SYNTH = ~/work/fpga/gatemate/oss-cad-suite/bin/yosys
	PR = ~/work/fpga/gatemate/oss-cad-suite/bin/nextpnr-himbaechel
	PACK = ~/work/fpga/gatemate/oss-cad-suite/bin/gmpack
	PROG = openFPGALoader -c $(CABLE)
else ifeq ($(BOARD), kolsch)
	FAMILY = gatemate
	DEVICE = ccgma1
	CABLE = dirtyJtag
	CCF = boards/kolsch_v2.ccf
	SYNTH = ~/work/fpga/gatemate/oss-cad-suite/bin/yosys
	PR = ~/work/fpga/gatemate/oss-cad-suite/bin/nextpnr-himbaechel
	PACK = ~/work/fpga/gatemate/oss-cad-suite/bin/gmpack
	PROG = openFPGALoader -c $(CABLE)
endif

FAMILY_UC = $(shell echo '$(FAMILY)' | tr '[:lower:]' '[:upper:]')

zeitlos: check zeitlos_pico bios soc os apps

ifeq ($(FAMILY), ice40)
zeitlos_pico: zeitlos_ice40_pico
else ifeq ($(FAMILY), ecp5)
zeitlos_pico: zeitlos_ecp5_pico
else ifeq ($(FAMILY), gatemate)
zeitlos_pico: zeitlos_gatemate_pico
endif

# Build logs. Both tools keep printing to the terminal; -l/--log
# additionally writes the FULL log to a file, which is what -q on the
# yosys line would otherwise throw away.
#
# Worth having because the things you need after a build are the things
# that scroll past during one: the post-pack utilisation table, and
# nextpnr's critical path report when a clock fails timing. Neither is
# in --report, which carries totals rather than the path.
#
#   $(OUTDIR)/synth.log   yosys: cell counts, inferred RAM/DSP,
#                         every "Warning:" the run produced
#   $(OUTDIR)/pnr.log     nextpnr: device utilisation, Max frequency
#                         per clock, and the critical path breakdown
#                         for each
#
# OUTDIR is output/<board> for an ordinary build; see its definition
# above. To find why a clock missed:
#   grep -A40 "Critical path report for clock" output/obst/pnr.log
#
# The logs are truncated per run, so what is in them always belongs to
# the bitstream sitting next to them.
SYNTH_LOG = $(OUTDIR)/synth.log
PNR_LOG = $(OUTDIR)/pnr.log

# ---------------------------------------------------------------------
# Synthesis as a real file, and a bitstream that says where it came from.
#
# zeitlos_ecp5_pico / bios / soc used to be command targets with no
# file prerequisites: naming one always ran its recipe, not naming it
# never did. That made `make soc` the packing step by design -- it
# consumed whatever soc.config sat in output/, and output/ is not in
# git. Two things then go wrong:
#
#   1. A change to RTL, boards.vh, the pin constraint, DEVICE, or
#      PNR_SEED is invisible to `make soc`. The common case is a
#      branch switch: git checkout of an older boards.vh does not
#      make that file newer, so make packs a netlist that no longer
#      matches the tree.
#   2. The resulting soc.bit carries no record of the commit, the
#      BIOS, or the seed that produced it. Two bitstreams of the
#      same name cannot be told apart.
#
# `make zeitlos` is still the full path (check, synth, bios, pack,
# os, apps). `make soc` is still the fast path when nothing changed.
# What is new is that packing without knowing is no longer possible.
#
# The stamp is the same pattern as sw/apps/net/Makefile's
# .net_config_selected: FORCE so the recipe always runs, but the
# file is only rewritten when BOARD/DEVICE/PACKAGE/EXTRA_DEFINES/
# ABC9/PNR_SEED actually changed, so repeating the same command
# does not look like a config change.
#
# The content hash is the other half. make compares mtimes; a hash
# of the synthesis inputs (RTL, boards.vh, csrs.vh, the constraint
# file, the stamp) does not. Written at the end of synthesis, and
# recomputed by `soc` before packing -- a mismatch resynthesizes
# even if every timestamp says not to.
#
# boards.vh and csrs.vh are `include'd from sysctl.v and are NOT
# in RTL_PICO, which is why they are listed separately. openssl
# rather than sha256sum/shasum so the same command works on macOS
# and Linux.
# ---------------------------------------------------------------------
SOC_CONFIG_STAMP = $(OUTDIR)/.soc_config
SOC_INPUTS_HASH  = $(OUTDIR)/soc.inputs.sha256

# File-level dependencies are wired for ecp5, which is what this
# Makefile can actually rebuild here. ice40 and gatemate keep their
# original command targets: the ice40 .pcf files are not in this
# tree, so listing them as prerequisites would make `make BOARD=keks`
# fail before nextpnr ever ran, and gatemate already packs inside
# zeitlos_gatemate_pico (`soc` is a no-op).
ifeq ($(FAMILY), ecp5)
SOC_CONSTRAINT = boards/$(LPF)
SOC_SYNTH_INPUTS = $(RTL_PICO) rtl/boards.vh rtl/csrs.vh $(SOC_CONSTRAINT)
endif

FORCE:

$(SOC_CONFIG_STAMP): FORCE
	@mkdir -p $(OUTDIR)
	@cur="BOARD=$(BOARD) DEVICE=$(DEVICE) PACKAGE=$(PACKAGE) EXTRA_DEFINES=$(EXTRA_DEFINES) ABC9=$(ABC9) PNR_SEED=$(PNR_SEED)"; \
	if [ ! -f $@ ] || [ "$$(cat $@)" != "$$cur" ]; then \
		echo "$$cur" > $@; \
	fi

# Default `bios` target also builds bios_seed.hex, which ecpbram/icebram
# need. Recursing without a target is what the old `bios:` recipe did.
sw/bios/bios.hex: FORCE
	$(MAKE) -C sw/bios BOARD=$(BOARD_UC) FAMILY=$(FAMILY_UC) PREFIX=$(PREFIX)

bios: sw/bios/bios.hex

ifeq ($(FAMILY), ice40)
zeitlos_ice40_pico:
	mkdir -p $(OUTDIR)
	yosys $(EXTRA_DEFINES) -DBOARD_$(BOARD_UC) -DICE40 -q -l $(SYNTH_LOG) -p \
		"synth_ice40 -top sysctl -json $(OUTDIR)/soc.json" $(RTL_PICO)
	nextpnr-ice40 --$(DEVICE) --package $(PACKAGE) --pcf boards/$(PCF) \
		--asc $(OUTDIR)/soc.txt --json $(OUTDIR)/soc.json \
		-l $(PNR_LOG) \
		--pcf-allow-unconstrained --opt-timing --ignore-loops
else ifeq ($(FAMILY), ecp5)
$(OUTDIR)/soc.config: $(SOC_SYNTH_INPUTS) $(SOC_CONFIG_STAMP)
	mkdir -p $(OUTDIR)
	yosys $(EXTRA_DEFINES) -DBOARD_$(BOARD_UC) -DECP5 -q -l $(SYNTH_LOG) -p \
		"synth_ecp5 $(ABC9) -top sysctl -json $(OUTDIR)/soc.json" $(RTL_PICO)
	nextpnr-ecp5 --$(DEVICE) --package $(PACKAGE) --lpf boards/$(LPF) \
		--json $(OUTDIR)/soc.json \
		--report $(OUTDIR)/report.txt \
		--textcfg $(OUTDIR)/soc.config \
		-l $(PNR_LOG) \
		$(if $(PNR_SEED),--seed $(PNR_SEED),) \
		--timing-allow-fail --ignore-loops
	@{ cat $(SOC_CONFIG_STAMP); cat $(SOC_SYNTH_INPUTS); } | openssl dgst -sha256 | awk '{print $$NF}' > $(SOC_INPUTS_HASH)
	@echo
	@grep -E "Max frequency for clock" $(PNR_LOG) | grep -v "ro_clk" | sed 's/^Info: //' || true
	@if grep "FAIL at" $(PNR_LOG) | grep -qv "ro_clk"; then \
		echo; \
		echo "*** TIMING NOT MET -- the bitstream will program and"; \
		echo "*** misbehave intermittently. Critical path:"; \
		echo; \
		awk '/Critical path report for clock/{c++} c' $(PNR_LOG) \
			| grep -E "Source|Sink|\.v:[0-9]" | head -30; \
		echo; \
		echo "*** full detail: make path BOARD=$(BOARD_LC)"; \
		echo; \
	fi

zeitlos_ecp5_pico: $(OUTDIR)/soc.config

# Content-hash gate. Separate from `soc` so the packing recipe does
# not contain $(MAKE): GNU make -n still executes any recipe line
# that invokes a sub-make, and packing from `make -n` would be a
# nasty surprise. This target always runs (FORCE); if the recorded
# hash does not match the tree, it rebuilds soc.config even when
# every mtime says the netlist is current.
$(OUTDIR)/.soc_inputs_ok: FORCE $(SOC_CONFIG_STAMP)
	@cur=`{ cat $(SOC_CONFIG_STAMP); cat $(SOC_SYNTH_INPUTS); } | openssl dgst -sha256 | awk '{print $$NF}'`; \
	reg=`cat $(SOC_INPUTS_HASH) 2>/dev/null`; \
	if [ -z "$$reg" ] || [ "$$cur" != "$$reg" ]; then \
		echo "soc: input hash $$cur does not match recorded $${reg:-<none>} -- resynthesizing"; \
		$(MAKE) --no-print-directory -B BOARD=$(BOARD) DEVICE=$(DEVICE) PACKAGE=$(PACKAGE) \
			EXTRA_DEFINES="$(EXTRA_DEFINES)" ABC9="$(ABC9)" PNR_SEED="$(PNR_SEED)" \
			PREFIX="$(PREFIX)" OUTDIR="$(OUTDIR)" $(OUTDIR)/soc.config; \
	fi
else ifeq ($(FAMILY), gatemate)
# Gatemate packs inside this target already (gmpack writes soc.bit);
# `soc` below stays a no-op. Left as a command target, same as before.
zeitlos_gatemate_pico:
	mkdir -p $(OUTDIR)
	$(SYNTH) $(EXTRA_DEFINES) -DBOARD_$(BOARD_UC) -DGATEMATE -q -l $(SYNTH_LOG) -p \
		"read -sv $(RTL_PICO); synth_gatemate -top sysctl -luttree -nomult \
			-nomx8 -json $(OUTDIR)/soc.json"
	$(PR) --device CCGM1A1 --json $(OUTDIR)/soc.json --vopt ccf=$(CCF) --vopt out=$(OUTDIR)/soc.txt --router router2 -l $(PNR_LOG)
	$(PACK) $(OUTDIR)/soc.txt $(OUTDIR)/soc.bit
endif

ifeq ($(FAMILY), ice40)
soc:
	icebram sw/bios/bios_seed.hex sw/bios/bios.hex < \
		$(OUTDIR)/soc.txt | icepack > $(OUTDIR)/soc.bit

else ifeq ($(FAMILY), gatemate)
soc:
	echo
else ifeq ($(FAMILY), ecp5)
soc: check $(OUTDIR)/soc.config $(OUTDIR)/.soc_inputs_ok sw/bios/bios.hex
	ecpbram -i $(OUTDIR)/soc.config \
		-o $(OUTDIR)/soc_final.config \
		-f sw/bios/bios_seed.hex \
		-t sw/bios/bios.hex
	ecppack -v --compress --freq 2.4 $(OUTDIR)/soc_final.config \
		--bit $(OUTDIR)/soc.bit
	@desc=`git describe --always --dirty 2>/dev/null || echo unknown`; \
	inhash=`cat $(SOC_INPUTS_HASH)`; \
	biosmd5=`openssl dgst -md5 sw/bios/bios.hex | awk '{print $$NF}'`; \
	bitmd5=`openssl dgst -md5 $(OUTDIR)/soc.bit | awk '{print $$NF}'`; \
	clk=`grep -E "Max frequency for clock" $(PNR_LOG) | grep -v ro_clk | grep clk48mhz | tail -1 | sed 's/^Info: //'`; \
	if [ -z "$$clk" ]; then clk=`grep -E "Max frequency for clock" $(PNR_LOG) | grep -v ro_clk | tail -1 | sed 's/^Info: //'`; fi; \
	printf '%s\n' \
		"git: $$desc" \
		"inputs: $$inhash" \
		"bios.md5: $$biosmd5" \
		"soc.bit.md5: $$bitmd5" \
		"BOARD=$(BOARD) DEVICE=$(DEVICE) PNR_SEED=$(PNR_SEED)" \
		"clk: $$clk" \
		> $(OUTDIR)/soc.bit.prov; \
	if echo "$$desc" | grep -q dirty; then \
		echo; \
		echo "*** DIRTY TREE -- $(OUTDIR)/soc.bit is not the bitstream of any commit."; \
		echo "*** git describe: $$desc"; \
		echo "*** Commit or stash before treating this file as a release artifact."; \
		echo; \
	fi; \
	echo "soc: $$desc  md5=$$bitmd5"
endif

ifeq ($(FAMILY), ice40)
flash_soc: check soc
	$(FLASH) $(OUTDIR)/soc.bit
else
flash_soc: check soc
	$(FLASH) $(OUTDIR)/soc.bit
endif

ifeq ($(FAMILY), ice40)
flash_os: check os
	$(FLASH) $(FLASH_OFFSET) sw/os/kernel.bin 100000
else
flash_os: check os
	$(FLASH) $(FLASH_OFFSET) 1048576 sw/os/kernel.bin
endif

# Core apps -- programmed at a fixed flash offset immediately ABOVE the
# kernel's 256KB region (1MB + 256KB = 0x140000). KEEP THIS OFFSET IN
# SYNC with Z_ZAR_FLASH_OFFSET in sw/os/zar.h; nothing checks that the
# two agree, and a mismatch looks like "no core apps in flash" rather
# than an error.
#
# Depends on `apps` so the .bin files exist; mkzar.py stores them
# verbatim (they are already ZEXE files).
#
# repl is NOT here. It and posix are the shells term connects to, and
# both live on the sdcard -- init() starts them from there when a card
# is present. See docs/flash_apps.md, "Why repl is not a core app".
$(OUTDIR)/apps.zar: apps
	mkdir -p $(OUTDIR)
	python3 tools/mkzar.py $(OUTDIR)/apps.zar \
		wm=sw/apps/wm/wm.bin \
		net=sw/apps/net/net.bin \
		term=sw/apps/term/term.bin

ifeq ($(FAMILY), ice40)
flash_apps: $(OUTDIR)/apps.zar
	$(FLASH) $(FLASH_OFFSET) $(OUTDIR)/apps.zar 140000
else
flash_apps: $(OUTDIR)/apps.zar
	$(FLASH) $(FLASH_OFFSET) 1310720 $(OUTDIR)/apps.zar
endif

# Boot splash logo -- programmed separately from the kernel, at a fixed
# flash offset immediately BELOW the kernel's own 1MB offset.
#
# It used to be compiled into kernel.bin as a 24KB const array
# (sw/os/logo_data.c). Since k_proc_create() sizes a process's memory
# block from its image, that cost 24KB of the 1MB main-memory budget
# permanently, for something shown once at boot. Flash is memory-mapped
# on this SOC (sw/bios/bios.c's load_zeitlos() memcpy()s the kernel
# straight out of it), so sw/os/logo.c now reads these bytes directly
# from flash into VRAM and no main memory is used at all.
#
# The artifact written here is sw/data/images/zeitlos_fb.bin: a full
# 640x480 1bpp framebuffer image, pre-centred and pre-padded from the
# 512x384 zeitlos.bin by sw/data/images/pad_logo.py. Doing the centring
# once, here, is what lets both the BIOS and the kernel show it with a
# single flat memcpy instead of a row-by-row copy -- and it makes the
# splash clear VRAM rather than leaving a garbage border. Regenerate
# with:
#
#   cd sw/data/images && python3 pad_logo.py zeitlos.bin zeitlos_fb.bin
#
# (add --invert if the splash shows with foreground/background swapped;
# polarity is baked into the image, not decided in C).
#
# No header, no wrapper, so there is nothing to keep in sync between the
# flashed bytes and what the BIOS/kernel expect to find -- and no
# is-it-programmed check anywhere: the logo is flashed alongside the
# gateware and kernel, so a board that can boot at all has it.
#
# 0xF0000 = 983040. The gateware lives at the start of flash (~400KB
# today, varies by board) and the kernel at 1MB, so this leaves the
# gateware headroom up to 960KB. Override both variables together if a
# board's gateware ever needs more than that:
#
#   make flash_logo LOGO_FLASH_OFFSET_HEX=e0000 LOGO_FLASH_OFFSET_DEC=917504
#
# KEEP THESE IN SYNC with Z_BOOT_LOGO_FLASH_OFFSET in sw/os/logo.h --
# there is no build-time link between them (that header is compiled into
# the kernel, these are arguments to an external flashing tool), so a
# mismatch shows up only as a missing or garbled splash at boot. The
# kernel skips drawing entirely if it finds erased flash there, so the
# failure mode is a blank screen rather than noise.
LOGO_FLASH_OFFSET_HEX ?= f0000
LOGO_FLASH_OFFSET_DEC ?= 983040

ifeq ($(FAMILY), ice40)
flash_logo: check
	$(FLASH) $(FLASH_OFFSET) sw/data/images/zeitlos_fb.bin $(LOGO_FLASH_OFFSET_HEX)
else
flash_logo: check
	$(FLASH) $(FLASH_OFFSET) $(LOGO_FLASH_OFFSET_DEC) sw/data/images/zeitlos_fb.bin
endif

prog: 
	$(PROG) $(OUTDIR)/soc.bit

dev: check clean_os clean_bios clean_apps os bios apps
dev-prog: dev soc prog
# Software-only reflash: kernel + core apps, leaving the gateware
# alone. The common development cycle, since RTL changes far less often
# than software does.
#
# flash_apps is included deliberately. kernel.bin and the core apps are
# coupled -- sw/common/syscalls.def is compiled into both, and an app
# built against a different one calls the wrong kernel handler for
# every syscall past the point they diverge (see that file's own
# warning). `dev` rebuilds both from clean, so flashing both together
# is what keeps them matched.
#
# NOTE this does not touch the bitstream. If you have changed anything
# under rtl/, use `make flash` instead -- software that expects
# hardware the running bitstream doesn't have fails in confusing ways
# (the one case that says so clearly is a CPU/ISA mismatch, which
# k_soc_report() catches at boot; everything else is on you).
dev-flash: dev flash_os flash_apps

# flash_logo included here so a full `make flash` still produces a
# system with a splash screen -- the extra write only costs setup time,
# and leaving it out would make the common path silently lose the logo.
# Flash it on its own (`make flash_logo`) when only the logo changed.
# flash_apps is part of the default `flash` deliberately: the point of
# putting the core apps in flash is that a freshly flashed board boots
# to a working desktop with no SD card at all. Leaving it as a separate
# opt-in target would defeat that -- a new user would flash the board,
# get a bare shell, and have no reason to suspect there was a second
# command to run. See sw/os/zar.h.
flash: zeitlos flash_soc flash_os flash_logo flash_apps

os:
	cd sw/os && make PREFIX=$(PREFIX)

apps:
	cd sw/apps && make PREFIX=$(PREFIX)

# Publish every built app to a TFTP root, so a running machine can pull
# the latest builds with `tget` instead of `xf` over the serial link.
# Each app lands under its bare name -- sw/apps/text/text.bin becomes
# $(TFTP_DIR)/text, which is what `tget` asks for and what `run <file>`
# expects to find afterwards.
#
#   $ make clean && sudo make BOARD=obst dev-flash && sudo make tftp-dist
#
# Deliberately does NOT depend on `apps`. This target is normally run
# under sudo (a TFTP root is usually root-owned), and a build running
# as root leaves root-owned .o files scattered through sw/, which then
# break every subsequent non-root build in the tree. Build first, copy
# second -- the script says so plainly if it finds nothing built.
TFTP_DIR ?= /srv/tftp

tftp-dist:
	@tools/tftp-dist.sh $(TFTP_DIR)

# Summarise the last place-and-route: one line per clock, and the
# critical path for any that failed.
#
# Runs automatically at the end of an ecp5 build, because a FAIL line
# is easy to miss in several thousand lines of nextpnr output and a
# bitstream that missed timing still programs and still half-works --
# which is a far more confusing thing to debug than one that refuses to
# build.
#
#   make timing BOARD=obst        after a build
#   make path BOARD=obst          full critical path for every clock
# ro_clk[*] are the TRNG's ring oscillators, each clocking one
# divide-by-two flop (see rtl/trng.v). nextpnr reports them as clock
# domains; they always pass and mean nothing here, so they are filtered.
timing:
	@test -f $(PNR_LOG) || { echo "no $(PNR_LOG) -- build first"; exit 1; }
	@echo
	@grep -E "Max frequency for clock" $(PNR_LOG) | grep -v "ro_clk" | sed 's/^Info: //' || true
	@if grep "FAIL at" $(PNR_LOG) | grep -qv "ro_clk"; then \
		echo; \
		echo "*** TIMING NOT MET -- the bitstream will program and"; \
		echo "*** misbehave intermittently. Critical path:"; \
		echo; \
		awk '/Critical path report for clock/{c++} c' $(PNR_LOG) \
			| grep -E "Source|Sink|\.v:[0-9]" | head -30; \
		echo; \
		echo "*** full detail: make path BOARD=$(BOARD_LC)"; \
		echo; \
	fi

# Differential test of the blitter: reference vs a candidate, same
# stimulus, framebuffers compared word for word. A clip-path change
# once hung the blitter on hardware (blank screen after wm, "blitter
# wait timed out" from zgfx) and nothing in the tree caught it, because
# nothing tested this module at all.
#
#   make test_blit                    reference against itself
#   make test_blit CAND=/tmp/new.v    reference against a candidate
# rtl/uart.v against itself: loopback at several line settings, FIFO
# depth, overrun, the divisor latch and both interrupt sources.
#
# Worth running before every flash that touches it, because this block
# answers the window sw/bios/bios.c writes its FIRST character into.
# A fault here does not degrade the console, it produces a board that
# prints nothing at all -- and the same fault in rtl/ext/uart16550
# would have been somebody else's bug, whereas this one is ours.
test_uart:
	@mkdir -p output
	iverilog -g2005 -o output/tb_uart rtl/tb/tb_uart.v rtl/uart.v
	@vvp output/tb_uart

# USB host controller: SIE, port front end, transaction engine and the
# Wishbone top, against a behavioural device model. Covers full speed,
# low speed wired directly (inverted polarity) and low speed behind a
# hub (normal polarity at the low-speed bit rate, every host packet
# preceded by a PRE) -- see docs/usb_host.md.
#
# The last two differing is the point of running both. A host that
# treats "low speed" as a single thing passes the direct case and fails
# the hub case, and the failure presents as a dead hub rather than as a
# polarity bug.
#
#   make test_usb                 run the suite
#   make test_usb TRACE=1         plus a packet-by-packet trace
TRACE ?= 0
test_usb:
	@mkdir -p output
	iverilog -g2005 $(if $(filter-out 0,$(TRACE)),-DUSB_TRACE,) \
		-o output/tb_usb_host rtl/tb/tb_usb_host.v \
		rtl/tb/tb_usb_device.v rtl/usb/usb_host.v rtl/usb/usb_sie.v \
		rtl/usb/usb_port.v rtl/usb/usb_xact.v rtl/usb/usb_hid_compat.v
	@vvp output/tb_usb_host

# CO-SIMULATION: the real sw/os/usb driver against the real rtl/usb
# gateware, joined by a VPI bridge (rtl/tb/cosim/usbh_vpi.c).
#
# This is the only test that exercises the two halves TOGETHER. The
# driver runs in a thread; its register accessors post a bus request
# and block, and the Verilog side runs a real Wishbone cycle against
# the DUT for each one. Simulation time is frozen while the driver
# computes, which is right -- that is code the CPU would have run
# between bus cycles, and the CPU is not modelled here.
#
# The driver is compiled with -DZ_USBH_COSIM and is otherwise the same
# source the kernel links.
#
#   make test_usb_cosim
#   make test_usb_cosim TRACE=1    plus every bus cycle
test_usb_cosim:
	@mkdir -p output
	cd output && iverilog-vpi --name=usbh_cosim -DZ_USBH_COSIM \
		-I../sw/os/usb ../rtl/tb/cosim/usbh_vpi.c \
		../sw/os/usb/usbh.c ../sw/os/usb/usbh_hid.c
	iverilog -g2005 $(if $(filter-out 0,$(TRACE)),-DCOSIM_TRACE,) \
		-o output/tb_usb_cosim rtl/tb/tb_usb_cosim.v \
		rtl/tb/tb_usb_device.v rtl/usb/usb_host.v rtl/usb/usb_sie.v \
		rtl/usb/usb_port.v rtl/usb/usb_xact.v rtl/usb/usb_hid_compat.v
	@cd output && vvp -M. -musbh_cosim tb_usb_cosim

# Utilisation of the USB host controller on its own, which is how the
# numbers in docs/usb_host.md were produced. CHECK THE DP16KD COUNT: if
# it is not 1, the packet buffer has fallen out of block RAM and into
# LUTs, which costs about 56000 of them and is reported by nothing. yosys only; nextpnr is not
# needed to count LUT4 and DP16KD, and pre-pack LUT4 is what
# docs/audio.md's table reports anyway.
#
#   make usb_area                 two-port build
#   make usb_area USB_PORTS=1     one-port build
USB_PORTS ?= 2
usb_area:
	@yosys -p "read_verilog rtl/usb/*.v; \
		chparam -set PORTS $(USB_PORTS) usb_host; \
		synth_ecp5 -abc9 -top usb_host; stat" 2>&1 \
		| sed -n '/=== usb_host ===/,$$p'

# Receive margin: how far a device's clock can be off before the host
# stops decoding it.
#
# USB allows +-2500 ppm at full speed and +-15000 ppm at low speed, and
# cheap devices sit near the edge. This runs the co-simulation harness
# against device models whose TRANSMIT clock is deliberately wrong, in
# both directions and at both speeds.
#
# It exists because the receiver was failing the full-speed spec limit
# and nothing could see it: tb_usb_device.v ran at exactly the nominal
# rate, so a receiver biased toward one side of the bit measured
# perfect. With CLK_PPM it showed +10000 ppm tolerance one way and
# -2500 the other -- all the margin on one side, failing at the spec
# limit on the other. See usb_sie.v's `mid`.
#
# A device model's own receiver is fixed-rate with no resync, far less
# tolerant than the host's DPLL, so CLK_PPM skews only its transmit
# path. Skewing both measures the testbench, not the design.
test_usb_margin:
	@mkdir -p output
	@cd output && iverilog-vpi --name=usbh_cosim -DZ_USBH_COSIM \
		-I../sw/os/usb ../rtl/tb/cosim/usbh_vpi.c \
		../sw/os/usb/usbh.c ../sw/os/usb/usbh_hid.c >/dev/null
	@for c in "10000 0" "-10000 0" "0 20000" "0 -20000"; do \
		set -- $$c; \
		iverilog -g2005 -DDEV_SKEW=20 -DDEV_PPM_FS=$$1 \
			-DDEV_PPM_LS=$$2 -o output/tb_usb_margin \
			rtl/tb/tb_usb_cosim.v rtl/tb/tb_usb_device.v \
			rtl/usb/*.v; \
		n=`cd output && vvp -M. -musbh_cosim tb_usb_margin 2>&1 \
			| grep -c '^  FAIL'`; \
		if [ "$$n" = "0" ]; then r="ok"; else r="FAIL ($$n)"; fi; \
		echo "  fs $$1 ppm, ls $$2 ppm: $$r"; \
	done

# Whole-SoC Fmax for a board, which is the number that actually decides
# whether a build is reliable.
#
# `usb_area` above reports LUT4 and DP16KD, and those were the only
# figures tracked while this controller was developed. Fmax was not,
# and it drifted from 52.5 MHz with `USB_HID to 45.3 MHz with
# `USB_HOST -- below the 48 MHz the design runs at -- without anything
# noticing. The symptom on hardware was intermittent receive CRC
# errors that moved between builds, which cost several days of
# chasing the USB receive path for a fault that was not there.
#
# Margin matters as much as passing: placement varies run to run, so a
# design that just scrapes 48 MHz will fail on some rebuilds and not
# others.
#
#   make usb_fmax BOARD=mozart_ml1
usb_fmax:
	@mkdir -p output
	@yosys $(EXTRA_DEFINES) -DBOARD_$(BOARD_UC) -DECP5 -q \
		-p "synth_ecp5 $(ABC9) -top sysctl -json output/fmax.json" \
		$(RTL_PICO)
	@nextpnr-ecp5 --$(DEVICE) --package $(PACKAGE) \
		--lpf boards/$(LPF) --json output/fmax.json \
		--textcfg /dev/null --lpf-allow-unconstrained 2>&1 \
		| grep -E "Max frequency|FAIL|Info: Device utilisation" \
		| head -20

CAND ?= rtl/gpu/gpu_blit.v
test_blit:
	@mkdir -p output
	@sed 's/^module gpu_blit_wb/module gpu_blit_cand/' $(CAND) \
		> output/gpu_blit_cand.v
	iverilog -g2005 -o output/tb_gpu_blit rtl/tb/tb_gpu_blit.v \
		rtl/gpu/gpu_blit.v output/gpu_blit_cand.v
	@vvp output/tb_gpu_blit

# Hardware map: every optional feature of rtl/sysctl.v on one A4 page,
# with reference tables and a check of define combinations that would
# not build or would hang the bus. No BOARD needed -- the map is not of
# any one board; each optional block is labelled with its define.
#
#   make hwmap                    output/docs/hwmap.{pdf,png,svg}
#   tools/hwmap/hwmap --check     just the checks
#
# See docs/hwmap.md.
hwmap:
	@tools/hwmap/hwmap

path:
	@test -f $(PNR_LOG) || { echo "no $(PNR_LOG) -- build first"; exit 1; }
	@awk '/Critical path report/{f=1} f' $(PNR_LOG)

# Utilisation, from the last build. The percentages are what decide
# whether the placer has room to do a good job -- above about 75%
# TRELLIS_COMB on this device it starts to struggle, and timing gets
# seed-sensitive.
util:
	@test -f $(PNR_LOG) || { echo "no $(PNR_LOG) -- build first"; exit 1; }
	@awk '/Device utilisation/{f=1} f && /Info:/' $(PNR_LOG) | head -20

clean: clean_os clean_bios clean_apps

# Removes EVERYTHING under output/, including output/releases/ if
# release/zrelease has built there. That is deliberate -- "clean the
# build output" should not leave some of it behind -- but it does mean
# a `make clean_soc` discards release bitstreams too. They rebuild.
clean_soc:
	rm -rf output/*

clean_os:
	cd sw/os && make clean

clean_bios:
	cd sw/bios && make clean

clean_apps:
	cd sw/apps && make clean

.PHONY: clean_bios bios apps tftp-dist timing path util test_blit test_uart hwmap FORCE soc
