# sw/apps/view -- build rules are shared: sw/common/app.mk, docs/build.md.

APP = view

# Which decoders to build in. Each is a compile-time switch in
# zimg.h so an app can pay only for what it uses -- see that file's
# Z_IMG_HAVE_* block.
#
# PNG IS ON. It costs about 33KB of .bss -- the inflate window is
# 32KB, but it shares zimg.c's decoder union with the others, so the
# real cost is the difference against the largest existing member.
#
# Turn it off with `make VIEW_PNG=0` on a board that cannot spare it;
# a PNG then reports "not supported" rather than failing to open.
#
# PNG matters more than its cost suggests: it is what diagrams,
# screenshots and logos on the web actually are, and line art survives
# 1bpp far better than a photograph does.
VIEW_PNG ?= 1

# SVG (sw/common/zsvg.c).
#
# On, because it is what view is BEST at: a diagram in one bit is
# nearly lossless, where a photograph is mush. Turn it off with
# `make VIEW_SVG=0` on a board that cannot spare the buffers below.
#
# VIEW_SVG_MAX is the document buffer -- SVG is text and the whole
# file is walked, since a transform on a <g> applies to children that
# appear later. VIEW_SVG_EDGES is the flattened outline of ONE shape;
# past it a drawing is refused rather than half-drawn.
VIEW_SVG       ?= 1
VIEW_SVG_MAX   ?= 65536
VIEW_SVG_EDGES ?= 2048

IMG_DEFS = -DZ_IMG_HAVE_PNG=$(VIEW_PNG) \
	-DVIEW_SVG=$(VIEW_SVG) \
	-DVIEW_SVG_MAX=$(VIEW_SVG_MAX) \
	-DVIEW_SVG_EDGES=$(VIEW_SVG_EDGES)

# Decode timing to UART0, off by default.
#
# Reports source size, output size and elapsed milliseconds for every
# image loaded. The point of it is the dithering-versus-decoding
# question: if dithering turns out to dominate, a hardware
# single-line ditherer becomes worth building, and if it doesn't, it
# very much isn't. Build with:
#
#     make VIEW_INSTRUMENT=1
#
# Costs nothing when off -- the whole block is #if'd out, and dbg()
# itself is dropped by --gc-sections if nothing else calls it.
VIEW_INSTRUMENT ?= 0
INSTR_DEFS = -DVIEW_INSTRUMENT=$(VIEW_INSTRUMENT)

# Cycle-level phase profiling (sw/common/zprof.h), off by default.
#
#     make VIEW_PROF=1
#
# Reports per-phase cycles, retired instructions and IPC to UART0 for
# every image loaded. This is the instrument that answers "where did
# the time go", which VIEW_INSTRUMENT above cannot: the KTIMER tick is
# 1.37ms and every phase worth optimising is shorter than that.
#
# ** This emits rdcycle/rdinstret. ** If a board is ever built with
# picorv32's ENABLE_COUNTERS(0) those become illegal instructions and
# this build will hang on the first image -- which is precisely why it
# is a separate flag and not always on. See zprof.h.
VIEW_PROF ?= 0
PROF_DEFS = -DZ_PROF=$(VIEW_PROF)

# Optimisation level for the decoders ONLY.
#
# Everything in this tree builds -Os, and for almost everything that
# is right: instruction fetch is the dominant term in CPI on this SOC
# (rtl/cache.v) so smaller code really is faster code.
#
# zimg.c's inverse DCT is the exception worth testing. It holds ~20
# live 32-bit values through the butterfly, and at -Os gcc spills
# rather than unrolls -- on a machine with no data cache every spill
# is a main-memory round trip at ~11 cycles (SDRAM) or ~63 (PSRAM).
# The profile showed ~7,400 instructions per 8x8 block against maybe
# 900 for a well-scheduled one, which is what spilling looks like.
#
#     make ZIMG_OPT=-O2 VIEW_PROF=1
#
# costs ~3KB of text. A/B it against the idct row of the profile
# before keeping it -- this is a hypothesis with a cheap experiment
# attached, not a known win, and -O2 could equally lose by pushing the
# hot loop out of the instruction cache.
ZIMG_OPT ?= -Os

APP_CFLAGS = $(IMG_DEFS) $(INSTR_DEFS) $(PROF_DEFS)

# zfsapp.o/zflist.o/zdialog.o are the file dialogs -- zflist.o needs
# zfsapp.o, and view.c needs both plus zdialog.o for the open dialog.
# zimg.o is the decoders themselves (sw/common/zimg.c), shared with
# any other app that wants to read images.

# Objects, and why each is linked:
#
#   zsvg.o: $(ZIMG_OPT) last so it overrides the -Os in CFLAGS.
#   sw/common/zinflate.c -- DEFLATE, which PNG's IDAT stream needs.
#   Linked unconditionally: it is small, and a VIEW_PNG=0 build simply
#   does not call it.
#   sw/common/zsvg.c -- vector rendering. Needs libm for the curve and
#   arc maths; the toolchain links it by default for this target.
#
#   zedit.o: sw/common/zedit.c -- the single-line text field.
#
#   zdialog.c owned it privately until sw/apps/web's URL bar became its
#   second caller. Every app that links the dialog needs this too: a
#   shared widget is only shared if the build says so.
OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zwin.o zwidget.o zspeak.o \
	zfsapp.o zflist.o zdialog.o zedit.o zimg.o zinflate.o zsvg.o \
	zprof.o view.o

zimg.o:
	$(CC) $(CFLAGS) $(ZIMG_OPT) -c ../../common/zimg.c -o zimg.o

include ../../common/app.mk
