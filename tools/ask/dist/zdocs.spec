# The Zeitlos documentation, on its own.
#
# ITS OWN PACK, not a component of the Ark packs, because it moves on a
# completely different schedule: docs/ changes every release and Ark
# changes once in a while. A pack owns its documents (see
# docs/ask_app.md), so bundling the two would mean rebuilding the whole
# Ark corpus to pick up a docs edit.
#
# Small enough that rebuilding it every release is free. This is the
# pack a Zeitlos release could reasonably ship by default.

include = common
dense   = no

version     = 1
name        = zdocs
description = Zeitlos documentation

source = mdtree ../../../docs  prefix=docs exclude=ask_app.md
