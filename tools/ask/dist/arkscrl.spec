# Ark Scroll -- the smallest Ark tier, one file.
#
# Named for Ark's own tier (Scroll / Lite / Medium / Heavy) rather than
# a parallel scale of our own. `arkscrl` because a card directory is
# 8.3 and `arkscroll` is nine.
#
# The Scroll is topic summaries rather than depth -- worth having when
# nothing else fits, and superseded by arklite when something does.

include = common

version     = 1
name        = arkscrl
description = Ark Scroll R1

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
generated   = scroll

source = scroll @ark/data/scroll-r1.gz
