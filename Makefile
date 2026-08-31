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
	rtl/debug.v \
	rtl/csrs.v \
	rtl/socctl.v \
	rtl/rtc.v \
	rtl/trng.v \
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
	rtl/ext/uart16550/rtl/verilog/uart_top.v \
	rtl/ext/uart16550/rtl/verilog/uart_wb.v \
	rtl/ext/uart16550/rtl/verilog/uart_debug_if.v \
	rtl/ext/uart16550/rtl/verilog/uart_defines.v \
	rtl/ext/uart16550/rtl/verilog/uart_regs.v \
	rtl/ext/uart16550/rtl/verilog/uart_rfifo.v \
	rtl/ext/uart16550/rtl/verilog/uart_tfifo.v \
	rtl/ext/uart16550/rtl/verilog/uart_sync_flops.v \
	rtl/ext/uart16550/rtl/verilog/uart_transmitter.v \
	rtl/ext/uart16550/rtl/verilog/uart_receiver.v \
	rtl/ext/uart16550/rtl/verilog/raminfr.v \
	rtl/usb_hid.v \
	rtl/ext/usb_hid_host/src/usb_hid_host.v \
	rtl/ext/usb_hid_host/src/usb_hid_host_rom.v

ifndef CABLE
	CABLE = dirtyJtag
endif

# Extra defines handed to the synthesis tool, on top of the
# -DBOARD_<X> and -D<FAMILY> the recipes below always pass.
#
# Empty for every ordinary build. It exists for release/zrelease, which
# passes -DZSPEC to make rtl/boards.vh use a generated define set
# instead of its own per-board `ifdef chain -- see that file's ZSPEC
# note for why a release needs to be able to build a board WITHOUT a
# feature, which a command-line -D cannot express.
EXTRA_DEFINES ?=

main: check zeitlos

check:
ifndef BOARD
	@echo must set BOARD variable \(make BOARD=obst\)
	@exit 1
endif

BOARD_LC = $(shell echo '$(BOARD)' | tr '[:upper:]' '[:lower:]')
BOARD_UC = $(shell echo '$(BOARD)' | tr '[:lower:]' '[:upper:]')

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
	DEVICE = 25k
	PACKAGE = CABGA381
	LPF = ulx3s.lpf
	PROG = openFPGALoader -c $(CABLE)
	FLASH = openFPGALoader -v -c $(CABLE) -f
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
#   output/<board>/synth.log   yosys: cell counts, inferred RAM/DSP,
#                              every "Warning:" the run produced
#   output/<board>/pnr.log     nextpnr: device utilisation, Max
#                              frequency per clock, and the critical
#                              path breakdown for each
#
# To find why a clock missed:
#   grep -A40 "Critical path report for clock" output/obst/pnr.log
#
# The logs are truncated per run, so what is in them always belongs to
# the bitstream sitting next to them.
SYNTH_LOG = output/$(BOARD_LC)/synth.log
PNR_LOG = output/$(BOARD_LC)/pnr.log

zeitlos_ice40_pico:
	mkdir -p output/$(BOARD_LC)
	yosys $(EXTRA_DEFINES) -DBOARD_$(BOARD_UC) -DICE40 -q -l $(SYNTH_LOG) -p \
		"synth_ice40 -top sysctl -json output/$(BOARD_LC)/soc.json" $(RTL_PICO)
	nextpnr-ice40 --$(DEVICE) --package $(PACKAGE) --pcf boards/$(PCF) \
		--asc output/$(BOARD_LC)/soc.txt --json output/$(BOARD_LC)/soc.json \
		-l $(PNR_LOG) \
		--pcf-allow-unconstrained --opt-timing --ignore-loops

zeitlos_ecp5_pico:
	mkdir -p output/$(BOARD_LC)
	yosys $(EXTRA_DEFINES) -DBOARD_$(BOARD_UC) -DECP5 -q -l $(SYNTH_LOG) -p \
		"synth_ecp5 -top sysctl -json output/$(BOARD_LC)/soc.json" $(RTL_PICO)
	nextpnr-ecp5 --$(DEVICE) --package $(PACKAGE) --lpf boards/$(LPF) \
		--json output/$(BOARD_LC)/soc.json \
		--report output/$(BOARD_LC)/report.txt \
		--textcfg output/$(BOARD_LC)/soc.config \
		-l $(PNR_LOG) \
		--timing-allow-fail --ignore-loops
	@$(MAKE) --no-print-directory timing

zeitlos_gatemate_pico:
	mkdir -p output/$(BOARD_LC)
	$(SYNTH) $(EXTRA_DEFINES) -DBOARD_$(BOARD_UC) -DGATEMATE -q -l $(SYNTH_LOG) -p \
		"read -sv $(RTL_PICO); synth_gatemate -top sysctl -luttree -nomult \
			-nomx8 -json output/$(BOARD_LC)/soc.json"
	$(PR) --device CCGM1A1 --json output/$(BOARD_LC)/soc.json --vopt ccf=$(CCF) --vopt out=output/$(BOARD_LC)/soc.txt --router router2 -l $(PNR_LOG)
	$(PACK) output/$(BOARD_LC)/soc.txt output/$(BOARD_LC)/soc.bit

bios:
	cd sw/bios && make BOARD=$(BOARD_UC) FAMILY=$(FAMILY_UC)

ifeq ($(FAMILY), ice40)
soc:
	icebram sw/bios/bios_seed.hex sw/bios/bios.hex < \
		output/$(BOARD_LC)/soc.txt | icepack > output/$(BOARD_LC)/soc.bit

else ifeq ($(FAMILY), gatemate)
soc:
	echo
else ifeq ($(FAMILY), ecp5)
soc:
	ecpbram -i output/$(BOARD_LC)/soc.config \
		-o output/$(BOARD_LC)/soc_final.config \
		-f sw/bios/bios_seed.hex \
		-t sw/bios/bios.hex
	ecppack -v --compress --freq 2.4 output/$(BOARD_LC)/soc_final.config \
		--bit output/$(BOARD_LC)/soc.bit
endif

ifeq ($(FAMILY), ice40)
flash_soc: check soc
	$(FLASH) output/$(BOARD_LC)/soc.bit
else
flash_soc: check soc
	$(FLASH) output/$(BOARD_LC)/soc.bit
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
output/$(BOARD_LC)/apps.zar: apps
	mkdir -p output/$(BOARD_LC)
	python3 tools/mkzar.py output/$(BOARD_LC)/apps.zar \
		wm=sw/apps/wm/wm.bin \
		net=sw/apps/net/net.bin \
		repl=sw/apps/repl/repl.bin \
		term=sw/apps/term/term.bin

ifeq ($(FAMILY), ice40)
flash_apps: output/$(BOARD_LC)/apps.zar
	$(FLASH) $(FLASH_OFFSET) output/$(BOARD_LC)/apps.zar 140000
else
flash_apps: output/$(BOARD_LC)/apps.zar
	$(FLASH) $(FLASH_OFFSET) 1310720 output/$(BOARD_LC)/apps.zar
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
	$(PROG) output/$(BOARD_LC)/soc.bit

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
	cd sw/os && make

apps:
	cd sw/apps && make

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
timing:
	@test -f $(PNR_LOG) || { echo "no $(PNR_LOG) -- build first"; exit 1; }
	@echo
	@grep -E "Max frequency for clock" $(PNR_LOG) | sed 's/^Info: //' || true
	@if grep -q "FAIL at" $(PNR_LOG); then \
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
CAND ?= rtl/gpu/gpu_blit.v
test_blit:
	@mkdir -p output
	@sed 's/^module gpu_blit_wb/module gpu_blit_cand/' $(CAND) \
		> output/gpu_blit_cand.v
	iverilog -g2005 -o output/tb_gpu_blit rtl/tb/tb_gpu_blit.v \
		rtl/gpu/gpu_blit.v output/gpu_blit_cand.v
	@vvp output/tb_gpu_blit

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

clean_soc:
	rm -rf output/*

clean_os:
	cd sw/os && make clean

clean_bios:
	cd sw/bios && make clean

clean_apps:
	cd sw/apps && make clean

.PHONY: clean_bios bios apps tftp-dist timing path util test_blit
