# sim/sys -- the Zeitlos SoC, emulated (docs/sim_parity.md).
#
#   make            zsys, the headless front end
#   make test       the core's own test programs, the devices' registers,
#                   a boot to the prompt, then to the desktop
#
#   ZSYS_CARD=images/zeitlos.img make test-desktop
#                   the desktop with a card too: welcome, from its first boot
#
# The tests need the RISC-V binutils (RISCV_PREFIX, as in
# sw/common/arch.mk) to assemble rtl/cpu/zeitlos32*/tests/prog, and the
# boot needs a built tree: the BIOS (sw/bios), the kernel (sw/os) and
# the core apps' archive (make flash_apps). Each is skipped, and says
# so, when it is not there.

CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -std=gnu99

ROOT = ../..
include $(ROOT)/sw/common/arch.mk

all: zsys

# With SDL2 (pkg-config sdl2), zsys has --window; without, it is headless.
SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null)
SDL_LIBS := $(shell pkg-config --libs sdl2 2>/dev/null)
ifneq ($(SDL_LIBS),)
WIN_SRC = win_sdl.c
WIN_FLAGS = -DZSYS_WINDOW $(SDL_CFLAGS)
endif

# T2: its own program and the raster's model, the two the RTL is held to
T2_SRC = zt2.c $(ROOT)/sw/common/zgl/t2fw.c $(ROOT)/sw/common/zgl/tests/t2_model.c \
	$(ROOT)/sw/common/zgl/tests/t2_raster.c
DEV_SRC = zsys.c zdev.c $(T2_SRC)

zsys: main.c $(DEV_SRC) zsys.h win.h $(WIN_SRC)
	$(CC) $(CFLAGS) $(WIN_FLAGS) -o $@ main.c $(DEV_SRC) $(WIN_SRC) $(SDL_LIBS)

# -- the core's test programs, as tb_zeitlos32.v runs them --

Z32 = $(ROOT)/rtl/cpu/zeitlos32/tests/prog
Z32P = $(ROOT)/rtl/cpu/zeitlos32p/tests/prog
TB_TESTS = $(Z32)/t_alu $(Z32)/t_branch $(Z32)/t_mem $(Z32)/t_muldiv $(Z32)/t_csr \
	$(Z32)/t_irq $(Z32)/t_illegal $(Z32P)/t_timer $(Z32P)/t_hazard
TB_OUT = out/tb

tb/%.bin: force
	@true

.PHONY: test test-screen test-tb test-zdev test-boot test-desktop test-input force
test: zsys test-tb test-zdev test-boot test-desktop test-input test-screen

# -- the devices, register by register (test_zdev.c) --

out/test_zdev: test_zdev.c $(DEV_SRC) zsys.h
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ test_zdev.c $(DEV_SRC)

test-zdev: out/test_zdev
	@./out/test_zdev

test-tb: zsys
	@if ! command -v $(RISCV_PREFIX)as >/dev/null; then \
		echo "test-tb: skipped -- no $(RISCV_PREFIX)as (set RISCV_PREFIX)"; exit 0; fi; \
	mkdir -p $(TB_OUT); fail=0; \
	for t in $(TB_TESTS); do n=$$(basename $$t); \
		cpp -I$(Z32) -P $$t.S -o $(TB_OUT)/$$n.i && \
		$(RISCV_PREFIX)as -march=rv32im -mabi=ilp32 -o $(TB_OUT)/$$n.o $(TB_OUT)/$$n.i && \
		$(RISCV_PREFIX)ld -m elf32lriscv -T $(Z32)/test.lds -o $(TB_OUT)/$$n.elf $(TB_OUT)/$$n.o && \
		$(RISCV_PREFIX)objcopy -O binary $(TB_OUT)/$$n.elf $(TB_OUT)/$$n.bin && \
		./zsys --tb $(TB_OUT)/$$n.bin > $(TB_OUT)/$$n.log 2>&1; r=$$?; \
		tail -1 $(TB_OUT)/$$n.log; [ $$r -eq 0 ] || fail=1; \
	done; exit $$fail

# -- a boot, BIOS to the kernel's prompt, headless --

BIOS = $(ROOT)/sw/bios/bios.bin
KERNEL = $(ROOT)/sw/os/kernel.bin
# the core apps' archive: where `make BOARD=<board> flash_apps` writes it
# (output/<board>/apps.zar; the same for every board), the newest
ZAR ?= $(shell ls -t $(ROOT)/output/apps.zar $(ROOT)/output/*/apps.zar 2>/dev/null | grep -v /releases/ | head -1)

test-boot: zsys
	@if [ ! -f $(BIOS) ] || [ ! -f $(KERNEL) ]; then \
		echo "test-boot: skipped -- build $(BIOS) and $(KERNEL) first"; exit 0; fi; \
	if [ ! -f "$(ZAR)" ]; then \
		echo "test-boot: skipped -- no core apps archive (make BOARD=<board> flash_apps, or ZAR=...)"; exit 0; fi; \
	mkdir -p out; \
	python3 mksimimg.py --bios $(BIOS) --kernel $(KERNEL) --zar "$(ZAR)" -o out/boot.img >/dev/null && \
	./zsys --flash out/boot.img --snapshot --fast --seconds 30 --type "ps\n" --after "init: done" \
		--expect "pid:  0" > out/boot.log 2>&1 && \
		echo "test-boot: the sim image boots to the prompt, and ps answers" || \
		{ echo "test-boot: FAILED (out/boot.log)"; tail -5 out/boot.log; exit 1; }; \
	python3 -c "import sys; d=bytearray(open('out/boot.img','rb').read()); d[0:16]=b'\\xff'*16; open('out/release.img','wb').write(d)" && \
	./zsys --bios $(BIOS) --flash out/release.img --snapshot --fast --seconds 30 --expect "init: done" > out/release.log 2>&1 && \
		echo "test-boot: a release-style image (gateware region, BIOS given) boots too" || \
		{ echo "test-boot: FAILED (out/release.log)"; tail -5 out/release.log; exit 1; }; \
	./zsys --flash out/release.img --seconds 1 > out/nobios.log 2>&1; [ $$? -eq 2 ] && \
		echo "test-boot: an image with no BIOS and no --bios is refused, and says why" || \
		{ echo "test-boot: FAILED (out/nobios.log)"; exit 1; }

# -- the desktop: wm on the GPU, a screenshot, each window's frame on it --
#
# Deterministic: the emulated machine's time, not the host's, so the same
# images give the same screen. ZSYS_CARD adds a boot with that card image
# (a snapshot: the file is not written), which welcome greets.

test-desktop: test-boot
	@if [ ! -f out/boot.img ]; then echo "test-desktop: skipped -- no out/boot.img"; exit 0; fi; \
	if [ ! -f "$(ZAR)" ]; then echo "test-desktop: skipped -- no core apps archive (make BOARD=<board> flash_apps), so no wm"; exit 0; fi; \
	./zsys --flash out/boot.img --snapshot --fast --seconds 3 --screenshot out/desktop.png > out/desktop.log 2>&1 && \
	python3 check_screen.py out/desktop.log out/desktop.png Dock welcome > out/desktop.check && \
		echo "test-desktop: wm draws the desktop and welcome, no card (out/desktop.png)" || \
		{ echo "test-desktop: FAILED (out/desktop.log, out/desktop.check)"; cat out/desktop.check; exit 1; }; \
	if [ -z "$(ZSYS_CARD)" ]; then echo "test-desktop: with a card skipped -- set ZSYS_CARD to a card image"; exit 0; fi; \
	./zsys --flash out/boot.img --sd "$(ZSYS_CARD)" --snapshot --fast --seconds 3 \
		--screenshot out/welcome.png > out/welcome.log 2>&1 && \
	grep -q "fs: sdcard mounted" out/welcome.log && \
	python3 check_screen.py out/welcome.log out/welcome.png welcome > out/welcome.check && \
		echo "test-desktop: the card mounts and welcome greets it (out/welcome.png)" || \
		{ echo "test-desktop: FAILED with the card (out/welcome.log, out/welcome.check)"; cat out/welcome.check; exit 1; }

# -- the screen size apps are told (docs/screen.md) --
#
# A card holding only /sys/zeitlos.cfg, with system.screen.size at the
# least there will be, 640x360 (16:9 in today's VRAM): the kernel must
# say it took it, and every window wm places -- the dock; then text,
# tapped on its dock icon, and the on-screen keyboard that opens for it
# with no keyboard plugged in -- must be inside it. The display stays
# 640x480, so what strays below row 359 is plain to see. The taps are
# where the dock is at 640x360; SCREEN=WxH for another size checks the
# first part.

SCREEN ?= 640x360

test-screen: test-boot
	@if [ ! -f out/boot.img ] || [ ! -f "$(ZAR)" ]; then echo "test-screen: skipped -- no out/boot.img or core apps archive"; exit 0; fi; \
	if ! command -v mkfs.fat >/dev/null || ! command -v mcopy >/dev/null; then \
		echo "test-screen: skipped -- needs mkfs.fat and mcopy (dosfstools, mtools)"; exit 0; fi; \
	rm -f out/screen.img; \
	mkfs.fat -C out/screen.img 8192 >/dev/null && \
	printf 'system.screen.size: $(SCREEN)\n' > out/screen.cfg && \
	mmd -i out/screen.img ::/sys && mcopy -i out/screen.img out/screen.cfg ::/sys/zeitlos.cfg && \
	./zsys --flash out/boot.img --sd out/screen.img --snapshot --fast --seconds 4 \
		--screenshot out/screen.png > out/screen.log 2>&1 && \
	grep -q "cfg: system.screen.size: apps are told $(SCREEN)" out/screen.log && \
	python3 check_screen.py --within $(SCREEN) out/screen.log out/screen.png Dock > out/screen.check && \
		echo "test-screen: told $(SCREEN), wm keeps every window inside it (out/screen.png)" || \
		{ echo "test-screen: FAILED (out/screen.log, out/screen.check)"; grep "screen\|cfg" out/screen.log | head; cat out/screen.check; exit 1; }; \
	./zsys --flash out/boot.img --sd out/screen.img --snapshot --fast --seconds 14 --no-keyboard --wait 6000 \
		--tap 100,330 --wait 4000 --screenshot out/screen-kb.png > out/screen-kb.log 2>&1 && \
	grep -q "no keyboard: the on-screen keyboard for untitled" out/screen-kb.log && \
	python3 check_screen.py --within $(SCREEN) --named out/screen-kb.log out/screen-kb.png keyboard > out/screen-kb.check && \
		echo "test-screen: a tap on text there, no keyboard: the on-screen keyboard fits it too (out/screen-kb.png)" || \
		{ echo "test-screen: FAILED (out/screen-kb.log, out/screen-kb.check)"; cat out/screen-kb.check; exit 1; }

# -- input: the USB keyboard and mouse, scripted; the flash's writes --
#
# The key/value store is written through the flash's write path: once
# welcome has been seen on a machine with no card, a second boot of the
# same flash image (a copy: not a snapshot) does not show it again.
# With ZSYS_CARD: Tab, Enter on welcome's first page opens the guide in
# `read`; Tab, Tab, Enter is Skip; a click on the dock starts `files`.

test-input: test-desktop
	@if [ ! -f out/boot.img ] || [ ! -f "$(ZAR)" ]; then echo "test-input: skipped -- no out/boot.img or core apps archive"; exit 0; fi; \
	cp out/boot.img out/kv.img && \
	./zsys --flash out/kv.img --fast --seconds 3 > out/kv1.log 2>&1 && \
	./zsys --flash out/kv.img --fast --seconds 3 > out/kv2.log 2>&1 && \
	grep -q "starting welcome" out/kv1.log && ! grep -q "starting welcome" out/kv2.log && \
		echo "test-input: the flash keeps the key/value store: welcome once, not twice" || \
		{ echo "test-input: FAILED (out/kv1.log, out/kv2.log)"; exit 1; }; \
	./zsys --flash out/boot.img --snapshot --fast --seconds 25 --no-keyboard --wait 9000 \
		--tap 421,322 --wait 1500 --tap 102,452 --wait 4000 --tap 309,375 --wait 300 --tap 379,341 \
		--wait 1000 --touch-down 200,27 --touch-move 230,60 --touch-move 260,110 --touch-up 260,110 \
		--wait 2500 --screenshot out/touch.png > out/touch.log 2>&1 && \
	grep -q "no keyboard: the on-screen keyboard for untitled" out/touch.log && \
	grep -q "win [0-9]* 'keyboard' pid [0-9]* at 16,277 608x203" out/touch.log && \
		echo "test-input: touch, no keyboard: taps close welcome and start text, and the keyboard opens for it (out/touch.png)" || \
		{ echo "test-input: FAILED: touch (out/touch.log)"; exit 1; }; \
	./zsys --flash out/boot.img --snapshot --fast --seconds 20 --wait 9000 \
		--click 421,322 --wait 1500 --click 102,452 --wait 3000 > out/touch-kbd.log 2>&1 && \
	grep -q "launched 'text'" out/touch-kbd.log && ! grep -q "no keyboard:" out/touch-kbd.log && \
		echo "test-input: with a keyboard plugged in, text opens and the on-screen keyboard does not" || \
		{ echo "test-input: FAILED: a keyboard there (out/touch-kbd.log)"; exit 1; }; \
	if [ -z "$(ZSYS_CARD)" ]; then echo "test-input: keyboard and mouse skipped -- set ZSYS_CARD to a card image"; exit 0; fi; \
	./zsys --flash out/boot.img --sd "$(ZSYS_CARD)" --snapshot --fast --seconds 4 --after "firstrun removed" \
		--keys '\t\n' --screenshot out/keys.png > out/keys.log 2>&1 && \
	python3 check_screen.py out/keys.log out/keys.png welcome read > out/keys.check && \
		echo "test-input: the keyboard: Tab, Enter in welcome opens the guide (out/keys.png)" || \
		{ echo "test-input: FAILED (out/keys.log, out/keys.check)"; cat out/keys.check; exit 1; }; \
	./zsys --flash out/boot.img --sd "$(ZSYS_CARD)" --snapshot --fast --seconds 4 --after "firstrun removed" \
		--keys '\t\t\n' --click 64,450 > out/click.log 2>&1 && \
	grep -q "destroyed window 1" out/click.log && grep -q "launched 'files'" out/click.log && \
		echo "test-input: Skip closes welcome, and a click on the dock starts files" || \
		{ echo "test-input: FAILED (out/click.log)"; exit 1; }; \
	if ! ./zsys --flash out/boot.img --sd "$(ZSYS_CARD)" --snapshot --fast --seconds 3 --after "boot finished" \
		--type "ls /apps/audiotest\n" 2>/dev/null | grep -q "^audiotest"; then \
		echo "test-input: audio skipped -- no audiotest on the card (the audio set)"; exit 0; fi; \
	./zsys --flash out/boot.img --sd "$(ZSYS_CARD)" --snapshot --fast --seconds 4 --after "firstrun removed" \
		--type "run /apps/audiotest\n" --wav out/audio.wav > out/audio.log 2>&1 && \
	python3 -c "import wave,struct,sys; w=wave.open('out/audio.wav'); r=w.getframerate(); n=w.getnframes(); \
a=struct.unpack('<%dh'%(2*n),w.readframes(n))[0::2]; s=a[int(1.5*r):int(2.5*r)]; \
z=sum(1 for i in range(1,len(s)) if s[i-1]<0<=s[i]); sys.exit(0 if 430<=z<=450 else 1)" && \
		echo "test-input: audio: audiotest's tone is 440 Hz in what the machine played (out/audio.wav)" || \
		{ echo "test-input: FAILED: audio (out/audio.log, out/audio.wav)"; exit 1; }; \
	./zsys --flash out/boot.img --sd "$(ZSYS_CARD)" --snapshot --fast --seconds 8 --after "firstrun removed" \
		--type "run /apps/mausrun3d\n" --stats > out/t2.log 2>&1; \
	if ! grep -q "mausrun3d: pid" out/t2.log; then echo "test-input: T2 skipped -- no mausrun3d on the card"; exit 0; fi; \
	grep -q "chunky: T2" out/t2.log && grep -q "T2 frames [1-9]" out/t2.log && \
		echo "test-input: T2: mausrun3d draws through the triangle engine (out/t2.log)" || \
		{ echo "test-input: FAILED: T2 (out/t2.log)"; exit 1; }

# -- the page: the same machine through Emscripten (docs/zsys.md, "In a browser") --
#
#   make web                     out/web/: index.html, zsys-page.js, zsys.js, zsys.wasm
#   make web-test ZSYS_CARD=...  the page in a headless Chromium (Playwright): to the
#                                desktop, a key and a click

EMCC ?= emcc
WEB = out/web
WEB_EXPORTS = _main,_zw_alloc,_zw_init,_zw_run,_zw_halted,_zw_render,_zw_rgba,_zw_dirty_lo,_zw_dirty_hi,_zw_key,_zw_mouse,_zw_wheel,_zw_touch,_zw_touch_wheel,_zw_touch_off,_zw_keyboard,_zw_audio_take,_zw_uart_in,_zw_card,_zw_card_written,_zw_card_blocks,_zw_flash,_zw_flash_size,_zw_flash_writes

.PHONY: web web-test
web: $(WEB)/zsys.js $(WEB)/index.html $(WEB)/zsys-page.js

$(WEB)/zsys.js: $(DEV_SRC) zsys.h web/zsys_web.c
	@mkdir -p $(WEB)
	$(EMCC) -O3 -std=gnu99 -Wno-unknown-attributes -o $@ $(DEV_SRC) web/zsys_web.c \
		-sMODULARIZE=1 -sEXPORT_NAME=ZsysModule -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=134217728 \
		-sEXPORTED_FUNCTIONS=$(WEB_EXPORTS) -sEXPORTED_RUNTIME_METHODS=HEAPU8

$(WEB)/%: web/%
	@mkdir -p $(WEB)
	cp $< $@

web-test: web
	@if [ ! -f out/boot.img ]; then echo "web-test: skipped -- no out/boot.img (make test-boot)"; exit 0; fi; \
	python3 web/test_page.py --web $(WEB) --flash out/boot.img $${ZSYS_CARD:+--card "$(ZSYS_CARD)"}

clean:
	rm -rf zsys out
