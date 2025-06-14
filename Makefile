RTL_PICO = \
	rtl/sysctl.v \
	rtl/clk/pll0.v \
	rtl/clk/pll1.v \
	rtl/cpu/picorv32/picorv32.v \
	rtl/mem/bram.v \
	rtl/mem/sram.v \
	rtl/mem/sdram.v \
	rtl/mem/qqspi.v \
	rtl/mem/vram.v \
	rtl/debug.v \
	rtl/spibb.v \
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

BOARD_LC = $(shell echo '$(BOARD)' | tr '[:upper:]' '[:lower:]')
BOARD_UC = $(shell echo '$(BOARD)' | tr '[:lower:]' '[:upper:]')

ifndef CABLE
	CABLE = usb-blaster
endif

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
	PROG = icoprog -p < output/soc.bin
	FLASH = icoprog -f < output/soc.bin
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
else ifeq ($(BOARD), kolsch_v0)
	FAMILY = gatemate
	DEVICE = ccgma1
	SYNTH = ~/work/fpga/gatemate/cc-toolchain-linux/bin/yosys/yosys
	PR = ~/work/fpga/gatemate/cc-toolchain-linux/bin/p_r/p_r
	PRFLAGS += -uCIO -ccf boards/kolsch_v0.ccf -cCP -crc +uCIO -om 2
else ifeq ($(BOARD), kolsch_v1)
	FAMILY = gatemate
	DEVICE = ccgma1
	SYNTH = ~/work/fpga/gatemate/cc-toolchain-linux/bin/yosys/yosys
	PR = ~/work/fpga/gatemate/cc-toolchain-linux/bin/p_r/p_r
	PRFLAGS += -uCIO -ccf boards/kolsch_v1.ccf -cCP -crc +uCIO -om 3
	PROG = openFPGALoader -c dirtyJtag
else ifeq ($(BOARD), kolsch_v2)
	FAMILY = gatemate
	DEVICE = ccgma1
	#SYNTH = ~/work/fpga/gatemate/cc-toolchain-linux/bin/yosys/yosys
	SYNTH = yosys
	PR = ~/work/fpga/gatemate/cc-toolchain-linux/bin/p_r/p_r
	PRFLAGS += -uCIO -ccf boards/kolsch_v2.ccf -cCP -crc +uCIO -om 3
	PROG = openFPGALoader -c dirtyJtag
else ifeq ($(BOARD), lowe)
	FAMILY = gatemate
	DEVICE = ccgma1
	SYNTH = ~/work/fpga/gatemate/cc-toolchain-linux/bin/yosys/yosys
	PR = ~/work/fpga/gatemate/cc-toolchain-linux/bin/p_r/p_r
	PRFLAGS += -uCIO -ccf boards/lowe.ccf -cCP -crc +uCIO -om 3
	PROG = openFPGALoader -c $(CABLE)
else ifeq ($(BOARD), lebkuchen)
	FAMILY = gatemate
	DEVICE = ccgma1
	CCF = boards/lebkuchen_v0.ccf
	SYNTH = ~/work/fpga/gatemate/oss-cad-suite/bin/yosys
	PR = ~/work/fpga/gatemate/oss-cad-suite/bin/nextpnr-himbaechel
	PROG = openFPGALoader -c $(CABLE)
else ifeq ($(BOARD), cceval)
	FAMILY = gatemate
	DEVICE = ccgma1
	SYNTH = ~/work/fpga/gatemate/cc-toolchain-linux/bin/yosys/yosys
	PR = ~/work/fpga/gatemate/cc-toolchain-linux/bin/p_r/p_r
	PRFLAGS += -uCIO -ccf boards/cceval.ccf -cCP -crc +uCIO -om 3
	PROG = openFPGALoader -c gatemate_evb_jtag
endif

FAMILY_UC = $(shell echo '$(FAMILY)' | tr '[:lower:]' '[:upper:]')

zeitlos: zeitlos_pico bios soc os apps

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
		"read -sv $(RTL_PICO); synth_gatemate -top sysctl -nomx8 -vlog output/$(BOARD_LC)/soc_synth.v"
	$(PR) --device $(FAMILY) \
		--json output/$(BOARD_LC)/soc.json \
		--report output/$(BOARD_LC)/report.txt \
		--textcfg output/$(BOARD_LC)/soc.config \
		--timing-allow-fail -o --ccf boards/$(CCF)
bios:
	cd sw/bios && make BOARD=$(BOARD_UC) FAMILY=$(FAMILY_UC)

ifeq ($(FAMILY), ice40)
soc:
	icebram sw/bios/bios_seed.hex sw/bios/bios.hex < \
		output/$(BOARD_LC)/soc.txt | icepack > output/$(BOARD_LC)/soc.bin
else ifeq ($(FAMILY), ecp5)
soc:
	ecpbram -i output/$(BOARD_LC)/soc.config \
		-o output/$(BOARD_LC)/soc_final.config \
		-f sw/bios/bios_seed.hex \
		-t sw/bios/bios.hex
	ecppack -v --compress --freq 2.4 output/$(BOARD_LC)/soc_final.config \
		--bit output/$(BOARD_LC)/soc.bin
endif

dev: clean_os clean_bios clean_apps os bios apps
dev-prog: dev soc prog

prog: 
	$(PROG) output/$(BOARD_LC)/soc.bin

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
