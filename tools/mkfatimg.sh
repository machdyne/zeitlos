#!/usr/bin/env bash
#
# Builds the SD card image by hand.
#
# NOTE: release/lib/mkfatimg.py builds the SAME image -- same mkfs.fat
# arguments, same file list, same fsck -- but populates it with mcopy
# instead of `mount -o loop`, so it needs no root. That matters here
# beyond convenience: a build run under sudo leaves root-owned .o files
# scattered through sw/, which then break every subsequent non-root
# build (the top-level Makefile's tftp-dist target has a comment about
# exactly this).
#
# Prefer:  release/zrelease build <version>
#
# This script is kept for the one-off case and as the readable
# reference for what goes on the card. If you change the file list
# here, change it in release/lib/mkfatimg.py too -- they are checked
# against each other by `release/zrelease check`.

set -euo pipefail

IMAGE="images/zeitlos.img"
SIZE_MB=64
MOUNT_DIR=$(mktemp -d)

cleanup()
{
    umount "$MOUNT_DIR" 2>/dev/null || true
    rmdir "$MOUNT_DIR"
}

trap cleanup EXIT

rm -f "$IMAGE" "$IMAGE.gz"

truncate -s "${SIZE_MB}M" "$IMAGE"

mkfs.fat \
    -F 32 \
    -S 512 \
    -s 1 \
    -n ZEITLOS \
    "$IMAGE"

mount -o loop "$IMAGE" "$MOUNT_DIR"

# we exclude core apps since they should exist on flash

# make directory skeleton
#
# apps/ joins docs/, ark/ and user/ rather than leaving a dozen
# executables loose in the root. sw/os/fs/fs.c's fs_exec_resolve()
# searches the root first and then apps/, so a bare `run term` still
# works and nothing above that function had to learn the new location.
mkdir "$MOUNT_DIR/apps"
mkdir "$MOUNT_DIR/audio"
mkdir "$MOUNT_DIR/docs"
mkdir "$MOUNT_DIR/ark"
mkdir "$MOUNT_DIR/user"

# libz/ holds the zcc runtime and the headers a program compiled ON the
# machine includes. See the block near the end of this file.
mkdir "$MOUNT_DIR/libz"
mkdir "$MOUNT_DIR/libz/include"

# supplemental apps
cp sw/apps/files/files.bin "$MOUNT_DIR/apps/files"
cp sw/apps/text/text.bin "$MOUNT_DIR/apps/text"
cp sw/apps/sheet/sheet.bin "$MOUNT_DIR/apps/sheet"
cp sw/apps/read/read.bin "$MOUNT_DIR/apps/read"
cp sw/apps/draw/draw.bin "$MOUNT_DIR/apps/draw"
cp sw/apps/info/info.bin "$MOUNT_DIR/apps/info"
cp sw/apps/calc/calc.bin "$MOUNT_DIR/apps/calc"
cp sw/apps/clock/clock.bin "$MOUNT_DIR/apps/clock"
cp sw/apps/cal/cal.bin "$MOUNT_DIR/apps/cal"
cp sw/apps/settings/settings.bin "$MOUNT_DIR/apps/settings"
cp sw/apps/track/track.bin "$MOUNT_DIR/apps/track"

# The two shells. init() starts both from the card; neither is in the
# flash archive, so these are the only copies. See docs/flash_apps.md.
cp sw/apps/repl/repl.bin "$MOUNT_DIR/apps/repl"
cp sw/apps/posix/posix.bin "$MOUNT_DIR/apps/posix"

# The self-hosting set: a compiler and an editor, driven from posix.
# These are what make the card able to extend itself rather than only
# run what was cross-compiled onto it. See docs/posix.md.
cp sw/apps/zcc/zcc.bin "$MOUNT_DIR/apps/zcc"
cp sw/apps/vi/vi.bin "$MOUNT_DIR/apps/vi"
cp sw/apps/ttytest/ttytest.bin "$MOUNT_DIR/apps/ttytest"

# games and demos
cp sw/apps/space3d/space3d.bin "$MOUNT_DIR/apps/space3d"
cp sw/apps/gamedemo/gamedemo.bin "$MOUNT_DIR/apps/gamedemo"
cp sw/apps/gpu3d/gpu3d.bin "$MOUNT_DIR/apps/gpu3d"

# misc
cp sw/apps/portdemo/portdemo.bin "$MOUNT_DIR/apps/portdemo"
# modules -- sw/apps/track scans /audio first, then the root
cp sw/data/audio/*.mod "$MOUNT_DIR/audio/"

# -- the zcc runtime --
#
# Two files and a pile of headers. A zcc that cannot find libz.bin
# produces a freestanding binary with no printf and no malloc; one that
# cannot find the headers cannot compile anything that includes them.
#
# This libz.bin is NOT the copy linked inside zcc.bin. That one is for
# the compiler's own use; this one is the runtime it EMBEDS into the
# programs it builds, and the two land at different processes'
# 0x8000_0000 so they cannot be shared. See docs/zcc.md.
#
# Headers are copied by directory rather than named one by one: the set
# is "whatever sw/common exports" and a list here would go stale
# silently, the symptom being a missing include on the device.
cp sw/apps/zcc/libz/libz.bin "$MOUNT_DIR/libz/libz.bin"
cp sw/apps/zcc/libz/libz.sym "$MOUNT_DIR/libz/libz.sym"
cp sw/apps/zcc/libz/libz.h "$MOUNT_DIR/libz/include/libz.h"

# The configuration template, every setting commented out -- see
# docs/config.md. At the root, where the kernel reads it.
cp sw/data/zeitlos.cfg "$MOUNT_DIR/zeitlos.cfg"
cp sw/common/syscalls.def "$MOUNT_DIR/libz/include/syscalls.def"
cp sw/common/*.h "$MOUNT_DIR/libz/include/"
cp sw/apps/zcc/include/*.h "$MOUNT_DIR/libz/include/"

cp docs/*.md "$MOUNT_DIR/docs/"
cp sw/data/ark/*.md "$MOUNT_DIR/ark/"

sync
ls -l "$MOUNT_DIR" "$MOUNT_DIR/apps" "$MOUNT_DIR/audio"
umount "$MOUNT_DIR"

fsck.fat -v "$IMAGE"

gzip "$IMAGE"
