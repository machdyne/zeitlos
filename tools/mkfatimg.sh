#!/usr/bin/env bash
#
# create a fatfs image containing zeitlos apps
#
# prerequisites:
#  apt install dosfstools
#
# usage:
#  chmod +x tools/mkfatimg.sh
#  sudo ./tools/mkfatimg.sh
#  sudo dd if=zeitlos.img of=/dev/sdX bs=4M status=progress conv=fsync
#

set -euo pipefail

IMAGE="zeitlos.img"
SIZE_MB=16

MOUNT_DIR="$(mktemp -d)"
LOOPDEV=""

cleanup() {
    set +e

    if mountpoint -q "$MOUNT_DIR"; then
        sync
        umount "$MOUNT_DIR"
    fi

    if [[ -n "$LOOPDEV" ]]; then
        losetup -d "$LOOPDEV" 2>/dev/null || true
    fi

    rmdir "$MOUNT_DIR"
}

trap cleanup EXIT

echo "Creating ${IMAGE} (${SIZE_MB} MiB)..."

# Create a zero-filled image.
truncate -s "${SIZE_MB}M" "$IMAGE"

# Create FAT filesystem.
mkfs.fat -F 32 "$IMAGE"

# Attach image to a loop device.
LOOPDEV=$(losetup --find --show "$IMAGE")

echo "Using loop device: $LOOPDEV"
echo "Mounting..."

mount "$LOOPDEV" "$MOUNT_DIR"

echo "Copying files..."

# ----------------------------------------------------------------------
# ZEITLOS DISTRIBUTION FILES
# ----------------------------------------------------------------------

cp sw/apps/hello_win/hello_win.bin "$MOUNT_DIR/hello"
cp sw/apps/net/net.bin "$MOUNT_DIR/net"
cp sw/apps/gpu3d/gpu3d.bin "$MOUNT_DIR/gpu3d"
cp sw/apps/term/term.bin "$MOUNT_DIR/term"
cp sw/apps/wm/wm.bin "$MOUNT_DIR/wm"
cp sw/apps/portdemo/portdemo.bin "$MOUNT_DIR/portdemo"

# ----------------------------------------------------------------------

sync

echo "Unmounting..."
umount "$MOUNT_DIR"

losetup -d "$LOOPDEV"
LOOPDEV=""

echo
echo "Done:"
ls -lh "$IMAGE"
