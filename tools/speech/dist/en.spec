# The English voice: every input, where it comes from, and under what
# terms. Read by tools/speech; read by people too, because this is the
# provenance record (tools/speech/README.md).
#
# EVERYTHING HERE IS PUBLIC DOMAIN, and that is a hard rule rather than
# a preference: the data ends up committed in this repository and
# copied onto every sdcard image. A source with no `licence` line is
# refused by the parser.
#
#   ./tools/speech/speech sources
#   ./tools/speech/speech fetch dist/en.spec
#
# URLs point at the ORIGINAL sources. See tools/speech/README.md, "Mirror the
# sources", for why that is temporary.

name        = en
description = English voice data for sw/apps/tts

# Nodes per letter in the trained letter-to-sound trees. This model is
# RESIDENT on the device, so the number is a memory decision:
#   300 -> 37KB, 35% of held-out words exactly right
#   600 -> 65KB, 41%
#  1200 -> 113KB, 46%
#  8000 -> 336KB, 50%
# The hand-written rules in lts.c, which this replaces when a pack is
# present, score 20% on the same test.
lts_nodes = 1200

# -- pronunciations, and the part of speech that disambiguates them --
#
# 177,267 entries in the Moby Pronunciator, plus the part-of-speech
# list, which is what tells "read" (present) from "read" (past) and
# "lead" (verb) from "lead" (metal).
#
# mobypron.unc is one entry per line, CR-terminated: the word, a space,
# then the pronunciation in Moby's own ASCII notation -- /eI/ for the
# vowel of "day", ' before the stressed syllable, _ between words.
# tools/speech converts that to our phoneme set (sw/apps/tts/phon.c).

source moby
    url      https://www.gutenberg.org/cache/epub/3205/pg3205.zip
    mirror   https://codeload.github.com/Hyneman/moby-project/tar.gz/refs/heads/master
    kind     zip
    mirror_kind tar.gz
    take     moby-project-master/moby/mpron/mobypron.unc -> mpron.txt
    take     moby-project-master/moby/mpos/mobyposi.i -> mpos.txt
    # NOT CMUDICT. The Moby distribution bundles a copy of it, and
    # CMUdict is BSD-licensed rather than public domain. It is the one
    # file in this project that must never be read by a build, so it is
    # named here and the extractor fails loudly if anything asks for
    # it. (It is fine as an ANSWER KEY on the build machine --
    # sw/apps/tts/tests/lts_eval.py scores against it -- because
    # nothing from it reaches the device.)
    refuse   moby-project-master/moby/mpron/cmudict0.3
    sha256   mpron.txt eab1c6dfda47178a36103041c118398c9a10ed1ce08c14d9d43865d60275b0d4
    sha256   mpos.txt daa369396e90e16ed8eb89b9e70e6b83939d021a7bd58077c82d3be7fe1a2d14
    licence  public domain -- Grady Ward, grant of January 2001; Project Gutenberg ebook #3205
    note     The checksums above are of the files as they appear in the GitHub mirror.
    note     The Gutenberg archive has a different layout; its `take` lines and hashes
    note     get filled in the first time somebody fetches without --mirror.

# -- recorded speech --
#
# 13,100 clips, about 24 hours, of one speaker reading seven
# non-fiction books. Used for alignment (durations, phrasing) and for
# the formant measurements the voice is fitted to. 2.6GB; `fetch
# --limit N` extracts only the first N clips, which is enough to
# develop against.
#
# Not needed until phase 7. Fetch it with --only ljspeech when you get
# there, rather than dragging it in for the pronunciation work.

source ljspeech
    url      https://data.keithito.com/data/speech/LJSpeech-1.1.tar.bz2
    kind     tar.bz2
    take     LJSpeech-1.1/metadata.csv -> metadata.csv
    take     LJSpeech-1.1/README -> README
    take     LJSpeech-1.1/wavs/ -> wavs
    sha256   metadata.csv 852bce5c79e5184ba5bf2ea98aad4bcf49a5d787def5fd1f04fa2ab8aa6dc535
    sha256   README bb2dae67dc465e830032fb5cfa5632a6cd69ba33395a6be57c3c59b4e6bd9386
    # The archive itself, as published: what `speech mirror` reports.
    note     Archive sha256 be1a30453f28eb8dd26af4101ae40cbf2c50413b1bb21936cbcdc6fae3de8aa5
    # A Machdyne-held copy goes here once it is published -- see
    # tools/speech/README.md, "Mirror the sources", and the `speech mirror`
    # command, which prepares one. A corpus this size published by one
    # person's web host is not something a build should depend on.
    #
    #   ./tools/speech/speech mirror ljspeech --clips 2000
    #
    # then one mirror_part line per published piece, in order:
    #   mirror_part https://github.com/machdyne/speech-data/releases/download/ljspeech-mirror/LJSpeech-1.1-2000.tar.bz2.part01
    #   mirror_part ...
    mirror_kind tar.bz2
    licence  public domain -- texts published 1884-1964; LibriVox recordings by Linda Johnson; alignment and annotation by Keith Ito, dedicated to the public domain
    note     Female speaker. Either the voice keeps that range or the formants are scaled.
    note     Derived from LibriVox, so it could be rebuilt from there if it ever vanished.

# -- word frequencies --
#
# Which 100k words go in the lexicon, and which get left to the rules,
# is decided by counting words in ordinary prose rather than by
# guessing. A few hundred Project Gutenberg books is plenty; the list
# below is a starting point and is expected to grow.

source books
    kind     books
    ids      1342 11 84 98 1661 2701 74 1400 345 2542 5200 1080 4300 2600 120
    ids      158 1260 768 219 16 35 36 164 174 205 215 236 244 408 514
    licence  public domain -- Project Gutenberg texts published before 1929
    note     Counted for word frequency only; no text is shipped.
