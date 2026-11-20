# sw/apps/dmatest -- build rules are shared: sw/common/app.mk, docs/build.md.
#
# A round trip through the filesystem, to check the DMA paths
# (docs/dma.md, "Testing"): writes a file of a known pattern, reads it
# back, compares, and says how long each took. Not in sw/apps/Makefile's
# APPS and not on any card: `make -C sw/apps/dmatest`, copy dmatest.bin
# over as /apps/dmatest, then `run dmatest`.

APP = dmatest

OBJS = zeitlos.o zobj.o dmatest.o

POSIX_IO = 1
APP_STACK = 128K

include ../../common/app.mk
