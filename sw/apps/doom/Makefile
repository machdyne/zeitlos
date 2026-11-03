# sw/apps/doom -- build rules are shared: sw/common/app.mk, docs/build.md.
#
# Doom (the shareware episode) on Zeitlos, full screen in chunky game
# mode. See docs/doom_app.md.
#
# Not in sw/apps/Makefile's APPS and not on any card:
#
#   make -C sw/apps/doom          fetch the engine and the WAD, build doom.bin
#
# then copy doom.bin to /apps/doom and doom1.wad to /data/doom/doom1.wad
# and `run doom`.
#
# Nothing GPL is in this tree. The engine is doomgeneric (GPL-2, the
# 1997 id release by way of Chocolate Doom), cloned at a pinned commit
# into sw/ext/doomgeneric by `make fetch` and compiled from there
# unmodified; doom.bin links it, so doom.bin is distributed under the
# GPL-2 with that source. This directory's own files are Zeitlos's.
# The WAD is id's shareware doom1.wad, from the Ubuntu archive's
# doom-wad-shareware package, checked by MD5. Both are .gitignore'd.

APP = doom

DG_URL    = https://github.com/ozkl/doomgeneric.git
DG_COMMIT = e257a09
DG_TOP    = ../../ext/doomgeneric
DG        = $(DG_TOP)/doomgeneric

WAD_DEB = doom-wad-shareware_1.9.fixed-4_all.deb
WAD_URL = http://archive.ubuntu.com/ubuntu/pool/multiverse/d/doom-wad-shareware/$(WAD_DEB)
WAD_MD5 = f0cefca49926d00903cf57551d901abe

# The engine's files, as upstream's own Makefile lists them, less its
# platform layers (doomgeneric_*.c), the stdio WAD reader (doom_zeitlos.c
# has its own, over the fs syscalls) and doomgeneric.c (doom_zeitlos.c
# owns main() and the screen buffer).
DG_SRC = dummy am_map doomdef doomstat dstrings d_event d_items d_iwad \
	d_loop d_main d_mode d_net f_finale f_wipe g_game hu_lib hu_stuff \
	info i_cdmus i_endoom i_joystick i_scale i_sound i_system i_timer \
	memio m_argv m_bbox m_cheat m_config m_controls m_fixed m_menu \
	m_misc m_random p_ceilng p_doors p_enemy p_floor p_inter p_lights \
	p_map p_maputl p_mobj p_plats p_pspr p_saveg p_setup p_sight \
	p_spec p_switch p_telept p_tick p_user r_bsp r_data r_draw r_main \
	r_plane r_segs r_sky r_things sha1 sounds statdump st_lib st_stuff \
	s_sound tables v_video wi_stuff w_checksum w_file w_main w_wad \
	z_zone i_input i_video
DG_OBJS = $(addprefix dg_,$(addsuffix .o,$(DG_SRC)))

APP_MAIN_RULE = 0
# zwin.o for z_launch_arg_take() alone: `run doom -fps` passes its
# words that way, not in argv (with zport, zgfx, zfont_data behind it)
OBJS = zeitlos.o zobj.o zport.o zwin.o zgfx.o zfont_data.o zfsapp.o \
	doom_zeitlos.o $(DG_OBJS)
POSIX_IO = 1
LDLIBS = -lm

# 8 MB, the most there is: Doom's zone is 6 MB of it.
APP_STACK = 8M

# 320x200, a byte a pixel: the engine's own screen, copied as it is.
DG_DEFS = -DCMAP256 -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 \
	-D_DEFAULT_SOURCE -DNORMALUNIX
APP_CFLAGS = -I../../common -I$(DG) $(DG_DEFS)

# Three engine functions are wrapped rather than edited (doom_zeitlos.c
# says why): where the configuration and saves go, how a file is found
# (also in the simulator, which has no fs process for stdio), and the
# frame going to the screen (straight from the engine's own buffer).
LDFLAGS_WRAP = -Wl,--wrap=M_SetConfigDir -Wl,--wrap=M_FileExists \
	-Wl,--wrap=I_FinishUpdate
LDLIBS += $(LDFLAGS_WRAP)

OWN_CFLAGS = $(CFLAGS) -Werror

.PHONY: fetch wad
fetch: $(DG)/doomgeneric.h wad

$(DG)/doomgeneric.h:
	rm -rf $(DG_TOP)
	git clone -q $(DG_URL) $(DG_TOP)
	cd $(DG_TOP) && git checkout -q $(DG_COMMIT)

wad: doom1.wad
doom1.wad:
	rm -rf wadpkg && mkdir wadpkg
	cd wadpkg && wget -q $(WAD_URL) || curl -sSfo $(WAD_DEB) $(WAD_URL)
	cd wadpkg && if command -v dpkg-deb >/dev/null; then \
		dpkg-deb --fsys-tarfile $(WAD_DEB) | tar x ./usr/share/games/doom/doom1.wad; \
	else \
		ar p $(WAD_DEB) data.tar.zst | zstd -dc | tar x ./usr/share/games/doom/doom1.wad; \
	fi
	echo "$(WAD_MD5)  wadpkg/usr/share/games/doom/doom1.wad" | md5sum -c --quiet
	mv wadpkg/usr/share/games/doom/doom1.wad doom1.wad
	rm -rf wadpkg

# The engine, unmodified: its warnings are upstream's. -O2, not -Os:
# speed is what it lacks, and code size is not short here.
dg_%.o: $(DG)/%.c | $(DG)/doomgeneric.h
	$(CC) $(CFLAGS) -O2 -w -c $< -o $@

doom_zeitlos.o: doom_zeitlos.c | $(DG)/doomgeneric.h
	$(CC) $(OWN_CFLAGS) -c doom_zeitlos.c -o doom_zeitlos.o

# `make` fetches what is missing, then builds
$(APP).elf: | fetch

CLEAN_EXTRA = wadpkg

include ../../common/app.mk

# On a first build the engine is not there yet, and make has already
# decided there is no rule for dg_*.o (their sources are missing). So
# fetch first, then build in a make that sees them.
ifeq ($(wildcard $(DG)/doomgeneric.h),)
.DEFAULT_GOAL := first
.PHONY: first
first: fetch
	$(MAKE) $(APP)
endif
