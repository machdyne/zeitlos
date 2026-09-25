# sw/apps/mesh -- a Meshtastic client. See docs/mesh_app.md.
#
#   make          the app, mesh.bin
#   make test     the host tests: zpb, the framer, the protocol, model
#                 and session layers against a simulated node, and the
#                 view logic (conversations, wrap, the input line), and
#                 the history file's records
#   make test_asan  the same under AddressSanitizer and UBSan
#
# zpb (sw/common/zpb.c) is tested here because mesh is its only user
# today. Run the tests after touching either.

COMMON = ../../common

PREFIX ?= $(RISCV_PREFIX)
# ARCH/ABI come from one place -- never hardcode them.
include $(COMMON)/arch.mk
CC = $(PREFIX)gcc

GC_SECTIONS ?= 1
ifeq ($(GC_SECTIONS),1)
GC_CFLAGS = -ffunction-sections -fdata-sections
GC_LDFLAGS = -Wl,--gc-sections
endif

# `override` so the flags this app REQUIRES survive a command-line
# CFLAGS, which would otherwise replace them outright.
override CFLAGS += --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=$(ABI) \
	-DZ_GFX_HW_BLIT -DZPORT_NO_PRINTF -I$(COMMON) $(GC_CFLAGS) $(ARCH_DEFS) $(LIBC_FLAGS)
LDFLAGS = -march=$(ARCH) -mabi=$(ABI) $(GC_LDFLAGS) $(LIBC_FLAGS)
LDSCRIPT = $(COMMON)/riscv-app.ld

# The core, shared by the app and the host tests.
CORE = mesh_frame mesh_model mesh_proto mesh_session mesh_view mesh_log
COMMON_OBJS = zeitlos.o zobj.o zport.o zrng.o zsoc.o zwin.o zgfx.o \
	zfont_data.o zrtc.o zcfg.o zcaption.o zpb.o zfsapp.o
OBJS = $(COMMON_OBJS) $(addsuffix .o,$(CORE)) mesh_log_io.o mesh_ui.o mesh.o

mesh: mesh.elf mesh.bin

$(COMMON_OBJS): %.o: $(COMMON)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

mesh_%.o: mesh_%.c
	$(CC) $(CFLAGS) -c $< -o $@

mesh.o: mesh.c
	$(CC) $(CFLAGS) -c mesh.c -o mesh.o

mesh.elf: $(OBJS)
	$(CC) $(LDFLAGS) -T $(LDSCRIPT) -Wl,-Map,mesh.map -o mesh.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble mesh.elf > mesh.dasm

# A .zexe: the header carries .bss as a number rather than as zeros on
# the card. See sw/common/zexec.h.
mesh.bin: mesh.elf
	@EDATA=$$($(PREFIX)nm mesh.elf | awk '$$3=="_edata"{print "0x"$$1}'); \
	END=$$($(PREFIX)nm mesh.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	$(PREFIX)objcopy -O binary mesh.elf mesh.data; \
	python3 ../../../tools/mkexec.py mesh.data mesh.bin $$(($$END - $$EDATA)); \
	rm -f mesh.data

# -DZ_GFX_HW_BLIT: text goes through the hardware glyph blitter, in
# z_font_5x8, which wm keeps in glyph memory (docs/window_manager.md,
# "Hardware glyph blitting").

# -- host tests --

HOSTCC ?= cc
HOSTFLAGS = -std=gnu99 -Wall -Wextra -Werror -I $(COMMON)
CORESRC = $(addsuffix .c,$(CORE)) $(COMMON)/zpb.c

test:
	$(HOSTCC) $(HOSTFLAGS) -o /tmp/mesh_test_zpb \
		$(COMMON)/tests/test_zpb.c $(COMMON)/zpb.c
	/tmp/mesh_test_zpb
	$(HOSTCC) $(HOSTFLAGS) -o /tmp/mesh_test_frame \
		tests/test_frame.c mesh_frame.c
	/tmp/mesh_test_frame
	$(HOSTCC) $(HOSTFLAGS) -o /tmp/mesh_test_session \
		tests/test_session.c $(CORESRC)
	/tmp/mesh_test_session
	$(HOSTCC) $(HOSTFLAGS) -o /tmp/mesh_test_view \
		tests/test_view.c mesh_view.c mesh_model.c
	/tmp/mesh_test_view
	$(HOSTCC) $(HOSTFLAGS) -o /tmp/mesh_test_log \
		tests/test_log.c $(CORESRC)
	/tmp/mesh_test_log

# The malformed-input and lossy-link tests only really check "stays
# inside the buffer" with these on.
test_asan:
	$(MAKE) test HOSTFLAGS="$(HOSTFLAGS) -fsanitize=address,undefined -g"

clean:
	rm -f mesh.elf mesh.bin mesh.data mesh.map mesh.dasm *.o *.d

.PHONY: mesh test test_asan clean render

-include *.d

# LOOK at the window before shipping a layout change -- see
# sw/common/tests/zrender.h for why an assertion cannot do this job.
#
#   make render [WHAT=chat|dm|log|help|connecting|noserial|small]
WHAT ?= chat
render:
	$(HOSTCC) -std=gnu99 -w -no-pie -I $(COMMON) -o /tmp/mesh_render \
		tests/render.c mesh_view.c mesh_model.c mesh_session.c \
		mesh_proto.c mesh_frame.c mesh_log.c $(COMMON)/zpb.c $(COMMON)/zwin.c \
		$(COMMON)/zfont_data.c $(COMMON)/zobj.c $(COMMON)/zeitlos.c \
		$(COMMON)/zrtc.c
	/tmp/mesh_render /tmp/mesh_$(WHAT).pbm $(WHAT)
