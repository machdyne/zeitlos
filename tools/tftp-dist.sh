#!/bin/sh
#
# Zeitlos
# Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
#
# Copies every built app binary into a TFTP server's root directory,
# so a running Zeitlos machine can pull the latest builds over the
# network with `tget` (docs/scheme_api.md, "Networking").
#
# Each app lands under its own bare name, no extension:
#
#     sw/apps/text/text.bin   ->   /srv/tftp/text
#
# which is what `tget` asks for and what `run <file>` expects to find
# on the SD card afterwards.
#
# USAGE
#
#     tools/tftp-dist.sh [dest-dir]
#
#   Normally invoked through the top-level Makefile, which passes
#   $(TFTP_DIR):
#
#     $ make clean && sudo make BOARD=obst dev-flash && sudo make tftp-dist
#
#   Override the destination with TFTP_DIR:
#
#     $ sudo make tftp-dist TFTP_DIR=/var/lib/tftpboot
#
# NOTES
#
#   - This does NOT build anything. That is deliberate: it is meant to
#     be run under sudo (a TFTP root is usually root-owned), and a
#     build running as root leaves root-owned .o files behind that
#     break every subsequent non-root build in the tree. Build first,
#     copy second.
#
#   - Only sw/apps/<name>/<name>.bin is copied -- a .bin whose name
#     doesn't match its directory isn't an app binary and is skipped,
#     rather than being published under a name nothing will ask for.
#
#   - The core apps (wm, net, repl, term) are copied too, even though
#     they are already in flash (sw/os/zar.h). Pulling a newer one
#     over the network is exactly how you'd test a change to it
#     without reflashing, and `run` prefers a file on the SD card
#     over the flash copy.

set -e

DEST="${1:-/srv/tftp}"

# Locate the repo root from this script's own location, so the script
# works regardless of the directory it's invoked from.
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)

APPS_DIR="$ROOT/sw/apps"

if [ ! -d "$APPS_DIR" ]; then
	echo "tftp-dist: no $APPS_DIR -- is this a Zeitlos tree?" >&2
	exit 1
fi

if [ ! -d "$DEST" ]; then
	echo "tftp-dist: $DEST does not exist" >&2
	echo "tftp-dist: create it, or pass another with TFTP_DIR=..." >&2
	exit 1
fi

if [ ! -w "$DEST" ]; then
	echo "tftp-dist: $DEST is not writable -- try sudo" >&2
	exit 1
fi

copied=0
skipped=0

for dir in "$APPS_DIR"/*/; do

	[ -d "$dir" ] || continue

	name=$(basename "$dir")
	bin="$dir$name.bin"

	if [ ! -f "$bin" ]; then
		skipped=$((skipped + 1))
		continue
	fi

	# Warn about a binary older than the ELF it came from. That means
	# a partial or failed build, and publishing it would hand out a
	# stale app that looks current -- an unpleasant thing to debug
	# from the far end of a TFTP transfer.
	elf="$dir$name.elf"
	if [ -f "$elf" ] && [ "$elf" -nt "$bin" ]; then
		echo "tftp-dist: WARNING: $name.bin is older than $name.elf (stale build?)"
	fi

	# Warn about a name the receiving end can't store under. FatFs is
	# built with FF_USE_LFN 0 (sw/os/fs/fatfs/ffconf.h), so the SD
	# card takes 8.3 SHORT names only -- a base name longer than 8
	# characters cannot be written there under that name.
	#
	# Not fatal, and the file is still published: `tget` takes an
	# optional local filename ((tget host remote local),
	# docs/scheme_api.md), so it can be fetched and saved as
	# something shorter. Worth saying out loud anyway, because the
	# two-argument form fails at the WRITE, after a successful
	# transfer -- which from the Zeitlos end reads as a network
	# problem rather than a filename one.
	if [ ${#name} -gt 8 ]; then
		echo "tftp-dist: NOTE: '$name' is ${#name} chars -- too long for the SD card's"
		echo "tftp-dist:       8.3 names; fetch it as 'tget <host> $name <shortname>'"
	fi

	cp -f "$bin" "$DEST/$name"
	chmod 644 "$DEST/$name"

	size=$(wc -c < "$bin" | tr -d ' ')
	printf 'tftp-dist: %-12s -> %s/%-12s (%s bytes)\n' \
		"$name.bin" "$DEST" "$name" "$size"

	copied=$((copied + 1))

done

if [ "$copied" -eq 0 ]; then
	echo "tftp-dist: nothing copied -- no app binaries found." >&2
	echo "tftp-dist: build them first, e.g. 'make apps'." >&2
	exit 1
fi

echo "tftp-dist: $copied app(s) copied to $DEST, $skipped app dir(s) not built"
