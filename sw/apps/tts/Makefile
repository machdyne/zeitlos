
PREFIX ?= $(RISCV_PREFIX)
# ARCH/ABI come from one place -- see that file for why
include ../../common/arch.mk
CC = $(PREFIX)gcc
AS = $(PREFIX)as
ASFLAGS = -march=$(ARCH) -mabi=$(ABI)
# Section GC, same as wm/clock: sw/common is linked a whole object at a
# time, and this drops the parts of zeitlos.o a service never calls.
GC_SECTIONS ?= 1
ifeq ($(GC_SECTIONS),1)
GC_CFLAGS = -ffunction-sections -fdata-sections
GC_LDFLAGS = -Wl,--gc-sections
endif
# `override` so the flags this app REQUIRES survive a command-line
# CFLAGS -- see sw/apps/hello_win/Makefile for the full story.
override CFLAGS += --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=$(ABI) $(ARCH_DEFS) $(LIBC_FLAGS) $(GC_CFLAGS)
LDFLAGS = -march=$(ARCH) -mabi=$(ABI) $(LIBC_FLAGS) $(GC_LDFLAGS)
LDSCRIPT = ../../common/riscv-app.ld

OBJS = zeitlos.o zobj.o zsoc.o zcfg.o zfsapp.o tts_queue.o text2ph.o pack.o dsyn.o lts.o phon.o synth.o tts_audio.o tts.o

tts: tts.elf tts.bin

zeitlos.o:
	$(CC) $(CFLAGS) -c ../../common/zeitlos.c -o zeitlos.o

zobj.o:
	$(CC) $(CFLAGS) -c ../../common/zobj.c -o zobj.o

zcfg.o:
	$(CC) $(CFLAGS) -c ../../common/zcfg.c -o zcfg.o

zsoc.o:
	$(CC) $(CFLAGS) -c ../../common/zsoc.c -o zsoc.o

# -O2 for the voice: synth.c's inner loop runs ~11,000 times a second
# per utterance, and -Os leaves speed on the table there.
synth.o: synth_tables.h

# The synthesiser's inner loops at -O2 rather than the rest of the
# app's -Os: they run 11,025 times a second, and on the target -O2 is
# 12% fewer instructions per sample for a few hundred bytes of code
# (measured by instruction count in an emulator; docs/tts.md,
# "Performance"). -O2 comes after -Os on the command line, so it wins.
synth.o: synth.c
	$(CC) $(CFLAGS) -O2 -c synth.c -o synth.o
	$(CC) $(CFLAGS) -O2 -c synth.c -o synth.o

phon.o:
	$(CC) $(CFLAGS) -c phon.c -o phon.o

zfsapp.o:
	$(CC) $(CFLAGS) -c ../../common/zfsapp.c -o zfsapp.o

pack.o:
	$(CC) $(CFLAGS) -c pack.c -o pack.o

# The recorded voice's inner loop runs per output sample: -O2, like synth.o.
dsyn.o: dsyn.c
	$(CC) $(CFLAGS) -O2 -c dsyn.c -o dsyn.o

lts.o:
	$(CC) $(CFLAGS) -c lts.c -o lts.o

text2ph.o:
	$(CC) $(CFLAGS) -c text2ph.c -o text2ph.o

tts_audio.o:
	$(CC) $(CFLAGS) -c tts_audio.c -o tts_audio.o

tts_queue.o:
	$(CC) $(CFLAGS) -c tts_queue.c -o tts_queue.o

tts.o:
	$(CC) $(CFLAGS) -c tts.c -o tts.o

tts.elf: $(OBJS)
	$(CC) $(LDFLAGS) -T $(LDSCRIPT) -Wl,-Map,tts.map -o tts.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble tts.elf > tts.dasm

tts.bin: tts.elf
	@EDATA=$$($(PREFIX)nm tts.elf | awk '$$3=="_edata"{print "0x"$$1}'); \
	END=$$($(PREFIX)nm tts.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	$(PREFIX)objcopy -O binary tts.elf tts.data; \
	python3 ../../../tools/mkexec.py tts.data tts.bin $$(($$END - $$EDATA)); \
	rm -f tts.data

# -- host-side tests --
#
# Native cc, no RISC-V toolchain. tts_queue.c is the REAL source; the
# test supplies the two callbacks. The zspeak.c client is tested here
# too, against a stubbed kernel, because the two halves of the
# protocol are easiest to reason about side by side. zsayall.c, the
# read-aloud pacer apps use for Super+A, is tested here for the same
# reason.
HOSTCC ?= cc
HOSTFLAGS = -std=gnu99 -Wall -Wextra -no-pie -I ../../common


# Instructions per audio sample on the target, by emulator (needs
# `pip install unicorn`). See tests/prof/prof.py.
prof:
	CROSS=$(patsubst %gcc,%,$(CC)) python3 tests/prof/prof.py

test:
	$(HOSTCC) $(HOSTFLAGS) -o /tmp/tts_test tests/tts_test.c tts_queue.c
	/tmp/tts_test
	$(HOSTCC) $(HOSTFLAGS) -o /tmp/zspeak_test tests/zspeak_test.c
	/tmp/zspeak_test
	$(HOSTCC) $(HOSTFLAGS) -o /tmp/zsayall_test tests/zsayall_test.c
	/tmp/zsayall_test
	$(HOSTCC) $(HOSTFLAGS) -o /tmp/text2ph_test tests/text2ph_test.c text2ph.c lts.c pack.c phon.c synth.c dsyn.c -DPACK_HOST
	/tmp/text2ph_test
	$(HOSTCC) $(HOSTFLAGS) -DPACK_HOST -o /tmp/pack_test tests/pack_test.c pack.c lts.c phon.c synth.c dsyn.c
	/tmp/pack_test

clean:
	rm -f tts.elf tts.bin tts.data tts.dasm tts.map *.o *.d

.PHONY: tts clean test

# Header dependency tracking -- see sw/apps/hello_win/Makefile.
-include $(wildcard *.d)

# -- the voice, on the build machine --
#
#   make wav                  render tests/testset.txt to /tmp/tts_wav/
#   make wav WPM=250          at another rate
#   python3 tests/asr_eval.py recogniser check of the single words
#
# synth_tables.h is generated (gen_tables.py) and checked in, so a
# build needs no Python; regenerate it if SYNTH_FS changes.

#   make wav PACK=../../../tools/speech/build/en/speech.zspk
#                             ...with the lexicon and trained rules
WPM ?= 180
PACK ?=
OUT ?= /tmp/tts_wav

wav:
	mkdir -p $(OUT)
	$(HOSTCC) $(HOSTFLAGS) -DSYNTH_STATS -DPACK_HOST -o /tmp/tts_wav_bin tests/tts_wav.c phon.c synth.c text2ph.c lts.c pack.c dsyn.c
	/tmp/tts_wav_bin tests/testset.txt $(OUT) $(WPM) $(PACK)

synth_tables.h: gen_tables.py
	python3 gen_tables.py > synth_tables.h

.PHONY: wav

# The letter-to-sound rules against a pronouncing dictionary (an
# answer key only -- see tests/lts_eval.py).
lts_eval:
	$(HOSTCC) $(HOSTFLAGS) -o /tmp/lts_cli tests/lts_cli.c lts.c
	python3 tests/lts_eval.py 2000 -v

.PHONY: lts_eval
