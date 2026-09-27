# sw/apps/mesh -- build rules are shared: sw/common/app.mk, docs/build.md.

APP = mesh
APP_LISTING = 0
# sw/apps/mesh -- a Meshtastic client. See docs/mesh_app.md.
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

include $(COMMON)/arch.mk

APP_CFLAGS = -DZPORT_NO_PRINTF -I$(COMMON)

# The core, shared by the app and the host tests.
CORE = mesh_frame mesh_model mesh_proto mesh_session mesh_view mesh_log
COMMON_OBJS = zeitlos.o zobj.o zport.o zrng.o zsoc.o zwin.o zgfx.o \
	zfont_data.o zrtc.o zcfg.o zcaption.o zpb.o zfsapp.o
OBJS = $(COMMON_OBJS) $(addsuffix .o,$(CORE)) mesh_log_io.o mesh_ui.o mesh.o

$(COMMON_OBJS): %.o: $(COMMON)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

mesh_%.o: mesh_%.c
	$(CC) $(CFLAGS) -c $< -o $@

# A .zexe: the header carries .bss as a number rather than as zeros on
# the card. See sw/common/zexec.h.

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

.PHONY: test test_asan render

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

include ../../common/app.mk
