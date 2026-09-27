# sw/apps/info -- build rules are shared: sw/common/app.mk, docs/build.md.

APP = info
APP_MAIN_RULE = 0

# Deliberately short, and shorter than it was: this app has no widgets,
# no dialogs, no file browsing and -- since the SD figure was removed
# (see info.c) -- no filesystem access at all. It reads a few syscalls
# and draws bars. logo_small.o is the generated wordmark bitmap.
OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zwin.o logo_small.o info.o

# logo_small.c/.h are generated from sw/data/images/zeitlos.bin by
# gen_logo_small.py -- see that script. Checked in, so building needs
# no Python.
logo_small.o: logo_small.c logo_small.h
	$(CC) $(CFLAGS) -c logo_small.c -o logo_small.o

# The feature-tag names, generated from zsoc.h so a new feature bit
# needs no edit here (gen_feat_names.awk; not committed). Rebuilt when
# zsoc.h changes.
feat_names.h: ../../common/zsoc.h gen_feat_names.awk
	awk -f gen_feat_names.awk ../../common/zsoc.h > feat_names.h

info.o: feat_names.h
	$(CC) $(CFLAGS) -c info.c -S -MF /dev/null -o info.asm
	$(CC) $(CFLAGS) -c info.c -o info.o

CLEAN_EXTRA = feat_names.h

include ../../common/app.mk
