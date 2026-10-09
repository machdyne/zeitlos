# Host test for sw/apps/zlink: two copies of zlinkapp.c, one per
# process, on simulated wires (test_zlink_app.c says what it covers).
#
#   make -C sw/apps/zlink/tests
#
# About 40 seconds: the link runs in real time, and the rate ladder
# (scenario 3) waits out two failed upgrades. Run it on an idle machine
# with two cores: the two simulated machines each want a CPU, and if
# they must take turns, every soft-zlink bit waits for a context switch
# -- a frame then takes longer than the link's 3 s dead time and the
# link drops, which on hardware (a CPU per machine) does not happen.
COMMON = ../../../common
CFLAGS = -std=gnu99 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers \
	-DZLINK_HOST_TEST -DZ_GS_HOST_TEST -I$(COMMON)

all:
	cc $(CFLAGS) -o /tmp/test_zlink_app test_zlink_app.c \
		$(COMMON)/zlink.c $(COMMON)/zlink_soft.c $(COMMON)/zlink_stream.c $(COMMON)/zgpio_stream.c
	/tmp/test_zlink_app
