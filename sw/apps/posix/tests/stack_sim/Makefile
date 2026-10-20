# Not an app: posix's builtins in job.c's coroutine on the RISC-V CPU,
# under the simulator, printing how much of the coroutine's stack each
# one used -- the machine's numbers that PX_JOB_STACK is sized from
# (docs/posix.md, "The command's stack"). The host tests (../run.sh)
# print the same table with x86-64 frames.
#
#   make -C sw/apps/posix/tests/stack_sim
#   sw/apps/posix/tests/run.sh        (builds the fixture in /tmp/posix-tests)
#   sim/zsim-headless -n 0 -r /tmp/posix-tests sw/apps/posix/tests/stack_sim/px_stack.bin
APP = px_stack
APP_STACK = 4M
APP_CFLAGS = -I../.. -I../../../../common
SRC_DIRS = ../.. ../../../../common/formats
# the simulator has no FS_STAT: px_stack.c answers it from FS_LIST
LDLIBS = -Wl,--wrap=fs_stat
OBJS = zeitlos.o zobj.o zfsapp.o zfsutil.o zwin.o zgfx.o zfont_data.o zinflate.o zgz.o \
	zf_core.o zf_xml.o zf_zip.o zf_arch.o zwalk.o zco.o \
	vfs.o sh.o tools.o tree.o job.o px_stack.o

include ../../../../common/app.mk
