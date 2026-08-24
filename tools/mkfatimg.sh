#!/usr/bin/env bash
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

cp sw/apps/net/net.bin "$MOUNT_DIR/net"
cp sw/apps/gpu3d/gpu3d.bin "$MOUNT_DIR/gpu3d"
cp sw/apps/term/term.bin "$MOUNT_DIR/term"
cp sw/apps/wm/wm.bin "$MOUNT_DIR/wm"
cp sw/apps/repl/repl.bin "$MOUNT_DIR/repl"
cp sw/apps/portdemo/portdemo.bin "$MOUNT_DIR/portdemo"

sync
umount "$MOUNT_DIR"

fsck.fat -v "$IMAGE"

gzip "$IMAGE"
