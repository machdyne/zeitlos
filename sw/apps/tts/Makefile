# sw/apps/tts -- build rules are shared: sw/common/app.mk, docs/build.md.

APP = tts
APP_LISTING = 0

OBJS = zeitlos.o zobj.o zsoc.o zcfg.o zfsapp.o tts_queue.o text2ph.o pack.o dsyn.o lts.o phon.o synth.o tts_audio.o tts.o

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

# The recorded voice's inner loop runs per output sample: -O2, like synth.o.
dsyn.o: dsyn.c
	$(CC) $(CFLAGS) -O2 -c dsyn.c -o dsyn.o

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

.PHONY: test

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

include ../../common/app.mk
