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
mkdir "$MOUNT_DIR/docs"
mkdir "$MOUNT_DIR/ark"
mkdir "$MOUNT_DIR/user"

# supplemental apps
cp sw/apps/files/files.bin "$MOUNT_DIR/files"
cp sw/apps/text/text.bin "$MOUNT_DIR/text"
cp sw/apps/read/read.bin "$MOUNT_DIR/read"
cp sw/apps/draw/draw.bin "$MOUNT_DIR/draw"
cp sw/apps/info/info.bin "$MOUNT_DIR/info"
cp sw/apps/calc/calc.bin "$MOUNT_DIR/calc"
cp sw/apps/clock/clock.bin "$MOUNT_DIR/clock"
cp sw/apps/settings/settings.bin "$MOUNT_DIR/settings"
cp sw/apps/track/track.bin "$MOUNT_DIR/track"

# games and demos
cp sw/apps/space3d/space3d.bin "$MOUNT_DIR/space3d"
cp sw/apps/gamedemo/gamedemo.bin "$MOUNT_DIR/gamedemo"
cp sw/apps/gpu3d/gpu3d.bin "$MOUNT_DIR/gpu3d"

# misc
cp sw/apps/portdemo/portdemo.bin "$MOUNT_DIR/portdemo"
cp docs/*.md "$MOUNT_DIR/docs/"
cp sw/data/ark/*.md "$MOUNT_DIR/ark/"

sync
ls -l "$MOUNT_DIR"
umount "$MOUNT_DIR"

fsck.fat -v "$IMAGE"

gzip "$IMAGE"
