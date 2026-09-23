# Ark Medium -- the largest set we intend to support.
#
# Ark defines Medium as <= 4GB, sized for a 4GB partition of a microSD
# or a DVD, and it includes everything below plus Ark Lite and the
# Scroll. This recipe is the whole thing.
#
#     ./tools/ask/ask fetch dist/arkmed.spec     # ~400MB, once
#     ./tools/ask/ask build dist/arkmed.spec
#
# The `fetch =` lines below do what ark's scripts/build.sh does, but
# from here, so a pack build does not depend on that script being
# correct on the day somebody runs it. As of ark 2650ce9 it has two
# bugs that leave Ark Medium incomplete and silent about it:
#
#   * `wget -nc -P $BUILD/ciaimg.zip <url>` -- `-P` is
#     --directory-prefix, so this creates a DIRECTORY called
#     ciaimg.zip and 7z is then handed a directory. The Factbook
#     images are never extracted. (The line above it correctly uses
#     -O.)
#   * `cp -R data/scroll.gz $ARK` -- there is no data/scroll.gz. The
#     repo has scroll-r0.gz and scroll-r1.gz, so Medium gets no
#     Scroll at all.
#
# Neither is fatal because the script has no `set -e`, which is how
# both survived. Both are upstream's to fix; this recipe sidesteps
# them.
#
# WHAT THIS ADDS over the script: resumable transfers, sha256 pinning,
# a real exit code, and a git-lfs pointer check that says what to do
# rather than failing inside `tar` with "File format not recognized".
#
# SIZE. This is hundreds of megabytes of text, and it ships as
# release/'s zeitlos-arkmedium.img.gz (docs/releases.md). The card
# holds all of it; the number to watch in the build output is
# "resident on device", not "total on card".
#
# LEXICAL ONLY. `dense = no`, as arklite and zdocs: BM25 alone
# measured better than every fused configuration (docs/ask_app.md,
# "The decision, and how it came out"), and a lexical pack holds
# NOTHING resident beyond two bytes a chunk of lengths -- which is the
# only reason a corpus this size fits the machine at all. With the
# dense half on, the coarse array alone would be ~8MB resident at
# 250,000 chunks and the build would run an SVD over all of them.
#
# The device side was made fit for this size in the same change:
# sw/apps/ask/aidx.c now streams every postings list in full and
# accumulates in bounded memory, rarest terms first. Before that it
# read only the first 4KB of each list -- invisible on arklite, whose
# lists fit, and at this size it would have scored only the start of
# the corpus for every common word. `sw/apps/ask/test/lexcheck.py
# --tile 20` is the check.

include = common

version     = 1
name        = arkmed
encoder     = bow
dense       = no
description = Ark Medium -- Wikipedia, Gutenberg, Factbook, MedlinePlus, Lite

# -- downloads --
#
# <dest> is relative to .cache/dl/arkmed/. With unpack (the default),
# it is the directory the archive expands into; unpack=no makes it the
# file.
#
# There are no `strip=` counts here. `strip=auto` is the default and
# drops ONE leading directory when every entry in the archive shares
# one -- which is what "unpack this into here" means, and which the
# Gutenberg CD zip (pg/), wiki2008.txz (wikipedia/) and the MedlinePlus
# tarball (medline/) all need. A hand-written number is a property of
# an archive nobody writing a recipe has necessarily downloaded yet;
# getting it wrong fails much later, as an adapter that cannot find its
# list file. ark's scripts/build.sh does the same job with
# `mv .../pg/* ...` and an rmdir, which misses dotfiles.
#
# sha256= is optional and unset here because these could not be
# verified from the machine this recipe was written on (gutenberg.org
# was not reachable). Pin them once you have fetched successfully: an
# unpinned download is one a mirror or a truncated transfer can change
# underneath you, and `ask fetch` cannot warn about a file it has
# nothing to compare against.
#
# PROJECT GUTENBERG HAS MOVED ITS DOWNLOAD PATHS. The old
# /files/<id>/<id>.txt form is gone; current is /ebooks/<id>.txt.utf-8
# (a redirect) or /cache/epub/<id>/pg<id>.txt (the file itself).
# `alt=` carries the second spelling so the recipe survives the next
# time one of them moves, rather than needing an edit.
#
# Note the dest for the Factbook text is an explicit FILENAME with
# unpack=no. The URL's own basename is "35830.txt.utf-8", which the
# `onefile` source below would not find -- naming the destination
# renames it on the way in.

fetch = gutenberg/cdrom https://www.gutenberg.org/files/11220/PG2003-08_files.zip

# The CD's title list is NOT in the zip. It is ark's own
# data/pgcdrom.lst, which scripts/build.sh copies in beside the texts
# (`cp $DATA/pgcdrom.lst $ARK/gutenberg/cdrom`) after moving pg/* up a
# level -- so its entries, `etext02/11001108.txt 1001 Nights`, are
# relative to the directory the zip unpacked into. `lfs=` names any
# file in a recipe repo, pointer or not, and `unpack=no` copies it to
# <dest> as a file. Without this line the `listed` source below fails
# with "pgcdrom.lst not found", which is how it was found.
fetch = gutenberg/cdrom/pgcdrom.lst lfs=ark/data/pgcdrom.lst unpack=no

fetch = gutenberg/cia/35830.txt https://www.gutenberg.org/ebooks/35830.txt.utf-8 alt=https://www.gutenberg.org/cache/epub/35830/pg35830.txt unpack=no

# The maps and flags. If this URL has moved too, the failure now names
# it rather than dying inside tarfile -- and an HTML error page served
# with a 200 is caught as an HTML error page.
fetch = gutenberg/cia https://www.gutenberg.org/cache/epub/35830/pg35830-images.zip alt=https://www.gutenberg.org/files/35830/35830-images.zip

# Wikipedia is a 137MB git-lfs object inside the ark repo, not a URL.
# `ask fetch` runs `git lfs pull` if it is still a pointer, and says
# what to install if git-lfs is missing -- rather than letting `tar`
# fail with "File format not recognized" several steps later.
fetch = wikipedia lfs=ark/data/wiki2008.txz

# MedlinePlus is a plain tarball in the repo. Unpacking it here keeps
# every Ark Medium component under one root.
fetch = medline lfs=ark/data/medline-2024-06-15.tgz

# Everything in the repo itself.
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
source = scroll @ark/data/scroll-r0.gz

# Everything scripts/build.sh fetches. `listed` reads a
# `relpath<space>title` list beside a directory of texts, which is the
# shape all of these share.
source = listed  @dl/medline   lst=medline.lst  prefix=medline
# The Wikipedia selection is HTML. `listed` converts it to markdown at
# build time (lib/html2md.py, on by default when a file sniffs as
# HTML; `html=no` turns it off). The card must carry readable text,
# not markup: a chunk is a byte range of the emitted file, so the
# index would otherwise hold `div`, `class` and `href` as search terms
# and a preview would quote tags. `read` renders the result.
source = listed  @dl/wikipedia lst=articles.lst prefix=wiki  split=262144
#
# `gutenberg=yes` strips the Project Gutenberg licence from the CD's
# etexts and from the Factbook. `books` does this by default; `listed`
# and `onefile` do not, because most of what they read is not
# Gutenberg's. Without it every one of the CD's books carries its
# small print into the index -- the defect docs/ask_app.md, "The
# corpus mattered more than the model", found to be worth more than
# any model change. The first arkmed build shipped without it.
source = listed  @dl/gutenberg/cdrom lst=pgcdrom.lst prefix=pg split=262144 gutenberg=yes
# The Factbook's sections are country lines, "@Afghanistan  (South
# Asia)", not chapters -- so `head=` names them and the dataset index
# becomes a list of countries. Without it the book was cut by size
# alone, which also let one country's entry straddle two documents and
# a preview stop mid-entry. The capture group is the label, so the `@`
# does not reach the reader. split= still bounds the handful of long
# entries (the United States, China).
source = onefile @dl/gutenberg/cia/35830.txt prefix=factbk split=131072 gutenberg=yes head=^@(.+)$ title="CIA World Factbook 2010"

# -- images --
#
# The Factbook's country maps and flags. These are NOT retrievable --
# an image has no text, so no embedding and no chunk, and `ask` will
# never return one as a hit. They get maps.md / flags.md instead, which
# `read` renders and which hand off to `view` on a click through
# ztype.h.
#
# Transcoded to GIF on the host, because sw/common/zimg.h compiles PNG
# OUT by default (Z_IMG_HAVE_PNG is 0 -- its inflate window alone is
# 32KB of .bss). GIF is built in, LZW-compressed and palette-based,
# which is what flat-colour flags and line-art maps want; JPEG is also
# built in but rings badly on hard edges, and BMP/PNM are uncompressed
# at 300KB per image. See lib/images.py.
#
# Classification is by filename pattern and is a GUESS: the archive's
# naming is not documented and was not reachable from the machine this
# recipe was written on. Anything matching neither lands in images.md,
# so a wrong guess costs a worse index rather than lost files. Check
# what actually appears and adjust CLASSIFY in lib/images.py.
images = @dl/gutenberg/cia
