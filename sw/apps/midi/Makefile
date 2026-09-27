# sw/apps/midi -- build rules are shared: sw/common/app.mk, docs/build.md.

APP = midi

# -- tuning knobs --

# Largest .MID accepted, in bytes.
#
# Loaded WHOLE, with no streaming. A MIDI file is a SCORE rather than
# audio -- ten to a hundred KB is typical -- and the format 1 track
# merge needs random access to every track at once anyway. 256KB covers
# everything short of a full orchestral export, and a larger file is
# refused with a message rather than truncated, because half a MIDI
# file plays perfectly right up until it stops.
MIDI_MAX_FILE ?= $(shell echo $$((256*1024)))

# Startup volume, 0..64. The 1-bit sigma-delta output drives headphones
# directly with no attenuator, so full scale is very loud -- the same
# warning sw/apps/track's MOD_VOLUME carries, for the same hardware.
MIDI_VOLUME ?= 40

APP_CFLAGS = -Wextra -DMIDI_MAX_FILE=$(MIDI_MAX_FILE) -DMIDI_VOLUME=$(MIDI_VOLUME)

# zfsapp.o is needed by zflist.o (the file browser inside the open
# dialog) as well as by midi.c's own file loading.

# Objects, and why each is linked:
#
#   zedit.o: sw/common/zedit.c -- the single-line text field.
#
#   zdialog.c owned it privately until sw/apps/web's URL bar became its
#   second caller. Every app that links the dialog needs this too: a
#   shared widget is only shared if the build says so.
OBJS = zeitlos.o zobj.o zsoc.o zgfx.o zfont_data.o zwin.o zwidget.o zspeak.o \
	zfsapp.o zflist.o zdialog.o zedit.o zkbd.o \
	smf.o synth.o midi_icons.o midi.o

# -- host-side tests for the sequencer and the synth --
#
# smf.c and synth.c include nothing from sw/common and touch no
# hardware, so the parsing, timing and pitch arithmetic can be checked
# on a machine where a wrong answer prints as a number instead of
# playing slightly wrong. Same idea, same reason, as
# sw/apps/track/modplay_test.c and sw/apps/play/play_test.c.
HOSTCC ?= cc
HOSTCFLAGS = --std=gnu99 -O2 -Wall -Wextra

midi_test: smf.c synth.c midi_test.c smf.h synth.h
	$(HOSTCC) $(HOSTCFLAGS) -o midi_test smf.c synth.c midi_test.c -lm

test: midi_test
	./midi_test

.PHONY: test

CLEAN_EXTRA = midi_test

include ../../common/app.mk
