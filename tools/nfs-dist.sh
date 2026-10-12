#!/bin/sh
#
# Zeitlos
# Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
#
# Copies every built app binary into an NFS export, as <export>/apps/<name>,
# so a Zeitlos machine with the export mounted at /nfs runs the latest
# build of an app by its bare name -- /nfs/apps is looked in first while
# system.apps.nfs_first is on (docs/nfs.md).
#
#     sw/apps/text/text.bin   ->   /srv/nfs/zeitlos/apps/text
#
# USAGE
#
#     tools/nfs-dist.sh [export-dir]
#
#   Normally invoked through the top-level Makefile, which passes
#   $(NFS_DIR):
#
#     $ make apps && make nfs-dist NFS_DIR=/srv/nfs/zeitlos
#
# NOTES
#
#   - This does NOT build anything (see tftp-dist.sh for why).
#
#   - Each file is written beside its final name and then renamed over
#     it, so a machine reading /nfs/apps/<name> while this runs sees the
#     old binary or the new one, never half of one. (NFS file handles
#     follow the inode: an app already being read keeps reading the old
#     file to its end.)
#
#   - The drivers (fs, usb, net) are copied like the rest but are never
#     run from /nfs: fs and usb are what /nfs is reached through, and net
#     is the NFS client (docs/nfs.md).

set -e

DEST="${1:-/srv/nfs/zeitlos}"

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
APPS_DIR="$ROOT/sw/apps"

if [ ! -d "$DEST" ]; then
	echo "nfs-dist: $DEST does not exist" >&2
	echo "nfs-dist: create it (and export it), or pass another with NFS_DIR=..." >&2
	exit 1
fi

mkdir -p "$DEST/apps"

if [ ! -w "$DEST/apps" ]; then
	echo "nfs-dist: $DEST/apps is not writable" >&2
	exit 1
fi

copied=0

for dir in "$APPS_DIR"/*/ "$APPS_DIR"/net/netserve/; do

	[ -d "$dir" ] || continue

	name=$(basename "$dir")
	bin="$dir$name.bin"
	[ -f "$bin" ] || continue

	elf="$dir$name.elf"
	if [ -f "$elf" ] && [ "$elf" -nt "$bin" ]; then
		echo "nfs-dist: WARNING: $name.bin is older than $name.elf (stale build?)"
	fi

	tmp="$DEST/apps/.$name.tmp"
	cp -f "$bin" "$tmp"
	chmod 644 "$tmp"
	mv -f "$tmp" "$DEST/apps/$name"

	size=$(wc -c < "$bin" | tr -d ' ')
	printf 'nfs-dist: %-12s -> %s/apps/%-12s (%s bytes)\n' \
		"$name.bin" "$DEST" "$name" "$size"

	copied=$((copied + 1))

done

if [ "$copied" -eq 0 ]; then
	echo "nfs-dist: nothing copied -- no app binaries found." >&2
	echo "nfs-dist: build them first, e.g. 'make apps'." >&2
	exit 1
fi

echo "nfs-dist: $copied app(s) copied to $DEST/apps"
