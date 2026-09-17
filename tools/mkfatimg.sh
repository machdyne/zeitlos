#!/usr/bin/env bash
#
# Builds the SD card image.
#
# This used to build the image itself, by hand, with `mount -o loop`
# and a file list of its own. release/lib/mkfatimg.py had a second copy
# of that list, and `zrelease check` compared the two -- which is what a
# duplicated list really costs: not just the drift, but a whole check
# whose only job is to police it. (It had drifted anyway. The shell list
# was missing gpudemo, chip8 and chess.)
#
# So this is a wrapper now. One file list, in release/lib/mkfatimg.py,
# and one implementation.
#
# The wrapper is kept rather than deleted because `tools/mkfatimg.sh` is
# what the docs and people's shell history already say, and because it
# is the obvious place to look for "how do I make a card".
#
# Populating with mcopy rather than a loopback mount is not just
# convenience: a build run under sudo leaves root-owned .o files
# scattered through sw/, which then break every subsequent non-root
# build. The top-level Makefile's tftp-dist target has a comment about
# exactly that.

set -euo pipefail

cd "$(dirname "$0")/.."

exec release/zrelease sdcard --gzip "$@"
