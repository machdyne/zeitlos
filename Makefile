RTL_PICO = \
	rtl/sysctl.v \
	rtl/clk/pll0.v \
	rtl/clk/pll1.v \
	rtl/cpu/picorv32/picorv32.v \
	rtl/mtu.v \
	rtl/arbiter.v \
	rtl/mem/bram.v \
	rtl/mem/sram.v \
	rtl/mem/sdram.v \
	rtl/mem/qqspi.v \
	rtl/mem/vram.v \
	rtl/spiflashro.v \
	rtl/debug.v \
	rtl/spibb.v \
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
	CABLE = usb-blaster
endif

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

zeitlos_ice40_pico:
	mkdir -p output/$(BOARD_LC)
	yosys -DBOARD_$(BOARD_UC) -DICE40 -q -p \
		"synth_ice40 -top sysctl -json output/$(BOARD_LC)/soc.json" $(RTL_PICO)
	nextpnr-ice40 --$(DEVICE) --package $(PACKAGE) --pcf boards/$(PCF) \
		--asc output/$(BOARD_LC)/soc.txt --json output/$(BOARD_LC)/soc.json \
		--pcf-allow-unconstrained --opt-timing

zeitlos_ecp5_pico:
	mkdir -p output/$(BOARD_LC)
	yosys -DBOARD_$(BOARD_UC) -DECP5 -q -p \
		"synth_ecp5 -top sysctl -json output/$(BOARD_LC)/soc.json" $(RTL_PICO)
	nextpnr-ecp5 --$(DEVICE) --package $(PACKAGE) --lpf boards/$(LPF) \
		--json output/$(BOARD_LC)/soc.json \
		--report output/$(BOARD_LC)/report.txt \
		--textcfg output/$(BOARD_LC)/soc.config \
		--timing-allow-fail

zeitlos_gatemate_pico:
	mkdir -p output/$(BOARD_LC)
	$(SYNTH) -DBOARD_$(BOARD_UC) -DGATEMATE -q -l synth.log -p \
		"read -sv $(RTL_PICO); synth_gatemate -top sysctl -luttree -nomult \
			-nomx8 -json output/$(BOARD_LC)/soc.json"
	$(PR) --device CCGM1A1 --json output/$(BOARD_LC)/soc.json --vopt ccf=$(CCF) --vopt out=output/$(BOARD_LC)/soc.txt --router router2
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

prog: 
	$(PROG) output/$(BOARD_LC)/soc.bit

dev: check clean_os clean_bios clean_apps os bios apps
dev-prog: dev soc prog
dev-flash: dev flash_os

flash: zeitlos flash_soc flash_os

os:
	cd sw/os && make

apps:
	cd sw/apps && make

clean: clean_os clean_bios clean_apps

clean_soc:
	rm -rf output/*

clean_os:
	cd sw/os && make clean

clean_bios:
	cd sw/bios && make clean

clean_apps:
	cd sw/apps && make clean

.PHONY: clean_bios bios apps
