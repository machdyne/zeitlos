# Ark Lite -- the Codex plus hand-selected texts, as Ark defines it.
#
# Everything here ships inside the ark repo, so this pack builds from a
# clone with no extra fetching. That makes it the one to develop
# against.
#
# split= on the books matters: read.c indexes lazily and only forward,
# so a hit in the back of an 8MB Bible means streaming 8MB off the card
# first. Splitting at chapter boundaries also measurably improved
# retrieval -- smaller documents carry tighter heading paths into the
# embedding.

include = common

version     = 1
name        = cmp
pack        = cmp
description = Ark Lite (Codex + selected texts) + Ark Scroll

# Datasets whose text is MODEL-WRITTEN rather than written by people.
#
# The Ark Scroll and the Ark Codex are both LLM output -- useful, and
# not a source. `ask` exists because a model small enough to run on
# this machine would confabulate; a corpus that is itself model-written
# has the same failure one step removed, and a reader deciding whether
# to trust a passage on water purification should be able to see
# whether it came from FM 21-76 or from a summary.
#
# Marked in the pack index (pack.py's DS_GENERATED), in the generated
# index.md, and in the manifest.
generated   = codex, scroll

source = codex  @ark/data/arklite/codex.tgz
source = books  @ark/data/arklite/books  split=262144
source = scroll @ark/data/scroll-r1.gz
