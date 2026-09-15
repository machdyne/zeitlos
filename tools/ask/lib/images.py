#
# Zeitlos ask -- images.
#
# Ark Medium ships the 2010 CIA World Factbook with its country maps
# and flags. Those are worth having on the card: a flag or a locator
# map is the kind of thing a reference corpus is FOR, and
# sw/apps/view already displays images.
#
# ============ WHY THESE ARE TRANSCODED, NOT COPIED ============
#
# sw/common/zimg.h compiles PNG OUT by default -- Z_IMG_HAVE_PNG is 0,
# because its inflate window alone is 32KB of .bss, more than every
# other decoder put together. BMP, PNM, GIF and JPEG are in.
#
# So a card full of PNGs would give "This format is not built in" on
# every one of them, and the fix would be rebuilding `view` with
# -DZ_IMG_HAVE_PNG=1 -- a device-side change to accommodate a host-side
# choice. Transcoding on the host is the cheaper direction.
#
# TARGET IS GIF, and the reasoning is worth writing down because the
# obvious answers are wrong:
#
#   PNG   not built in. Rejected above.
#   JPEG  built in, but these are flags and line-art maps -- flat
#         colour with hard edges, the worst case for DCT. Ringing
#         artefacts survive into the dither and look like noise.
#   BMP   built in, uncompressed. 640x480 greyscale is 300KB; a few
#         hundred of those is a hundred megabytes for pictures of
#         flags.
#   PNM   built in, also uncompressed. Same arithmetic.
#   GIF   built in, LZW-compressed, palette-based. Exactly what flat
#         colour art wants. A flag comes out at a few KB.
#
# view dithers to 1bpp at decode time anyway (zimg.c's Floyd-Steinberg),
# so the palette is converted to GREYS rather than kept in colour --
# giving the ditherer real luminance to work with instead of whatever a
# colour-to-grey conversion happens to produce.
#
# Images are also downscaled here rather than on the device. view
# downscales by powers of two at decode time into a fixed 640x480 1bpp
# document, so anything larger is decode work thrown away, and card
# space spent on pixels nobody sees.
#
# ============ WHAT IS NOT DONE HERE ============
#
# The images are NOT retrievable. An image has no text, so it has no
# embedding and no chunk, and `ask` will not return one as a hit. What
# it gets instead is an index.md of links -- see write_image_index() --
# which `read` renders and which hands off to `view` on a click,
# through ztype.h. Making the captions retrievable is a later thing and
# wants the captions to exist first; the Factbook's images are named,
# not captioned.
#

import os
import re

# Longest edge, in pixels. Z_IMG_MAX_W/H is 640x480 (zimg.h) and the
# document is a fixed 1bpp buffer of exactly that, so anything bigger
# is decoded and then thrown away.
MAX_EDGE = 640

# What the card gets. See the header for why not PNG, JPEG or BMP.
OUT_EXT = "gif"

# Greyscale levels in the output palette. The device dithers to 1bpp,
# so this only has to be enough for Floyd-Steinberg to work with; 64 is
# indistinguishable from 256 after dithering and compresses better.
GREYS = 64

SRC_EXTS = (".png", ".jpg", ".jpeg", ".gif", ".bmp", ".tif", ".tiff")

# How a Factbook image filename is classified. Pattern-driven because
# the archive's naming is not documented anywhere and may not match
# what this expects -- anything unmatched lands in "images" rather than
# being dropped, so a wrong guess costs a worse index, not lost files.
CLASSIFY = [
    ("flags", re.compile(r"flag", re.I)),
    ("maps", re.compile(r"(map|locator|region|world)", re.I)),
]
DEFAULT_CLASS = "images"


class ImageError(Exception):
    pass


# Files emit() could not read, as "name: reason". Reported by the
# build rather than raised, so one bad file in an archive of several
# hundred costs that file and nothing else.
skipped = []


class MissingPillow(Exception):
    """Pillow is not installed. A setup problem, not a bad file, so it
    is fatal where ImageError is merely reported."""
    pass


def classify(name):
    for label, rx in CLASSIFY:
        if rx.search(name):
            return label
    return DEFAULT_CLASS


def _pretty(name):
    """A human label from a filename, for the index."""
    stem = os.path.splitext(os.path.basename(name))[0]
    stem = re.sub(r"[_\-]+", " ", stem)
    stem = re.sub(r"\s*\b(flag|map|locator|large|small)\b\s*", " ", stem,
                  flags=re.I)
    stem = re.sub(r"\s+", " ", stem).strip()
    return stem.title() if stem else os.path.basename(name)


def find(root, exts=SRC_EXTS):
    out = []
    for dp, _dn, fn in os.walk(root):
        for f in sorted(fn):
            if f.lower().endswith(exts):
                out.append(os.path.join(dp, f))
    return sorted(out)


def transcode(src, dest, max_edge=MAX_EDGE, greys=GREYS):
    """Convert one image. Returns (width, height, bytes) or raises."""
    try:
        from PIL import Image
    except ImportError:
        raise MissingPillow(
            "image transcoding needs Pillow:\n"
            "    pip install -r tools/ask/requirements.txt\n"
            "  Or drop the `images` source from the recipe -- the text "
            "corpus does not need it.")

    try:
        im0 = Image.open(src)
    except Exception as e:
        # One unreadable file must not take the build down. An archive
        # of several hundred images will contain a truncated one
        # eventually, and losing the other 399 to it -- with a Pillow
        # traceback rather than a filename -- is the wrong trade.
        raise ImageError("%s: %s" % (os.path.basename(src), e))

    with im0 as im:
        im = im.convert("L")
        w, h = im.size
        if max(w, h) > max_edge:
            scale = float(max_edge) / max(w, h)
            im = im.resize((max(1, int(w * scale)), max(1, int(h * scale))),
                           Image.LANCZOS)
        # Quantise to a grey palette. GIF is palette-only, and going
        # through P mode explicitly keeps control of the level count
        # rather than letting the encoder pick.
        im = im.quantize(colors=greys, method=Image.MEDIANCUT)
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        im.save(dest, format="GIF", optimize=True)
        return im.size[0], im.size[1], os.path.getsize(dest)


def emit(srcs, outdir, pack, dataset="images", max_edge=MAX_EDGE,
         quiet=False):
    """Transcode `srcs` into the card tree under the given pack.

    Returns a list of {class, name, path, w, h, bytes}, where `path` is
    relative to the card root.
    """
    from . import cardfs

    groups = {}
    for s in srcs:
        groups.setdefault(classify(os.path.basename(s)), []).append(s)

    out = []
    del skipped[:]
    for label in sorted(groups):
        base = "%s/%s/%s" % (cardfs.CARD_ROOT, pack, label)
        cardfs._check_83(label)
        n = 0
        for src in sorted(groups[label]):
            n += 1
            sub = ""
            if cardfs.SPLIT_AT and n > cardfs.SPLIT_AT:
                sub = "/%03d" % ((n - 1) // cardfs.SPLIT_AT)
            rel = "%s%s/%08d.%s" % (base, sub, n, OUT_EXT)
            try:
                w, h, nbytes = transcode(src, os.path.join(outdir, rel),
                                         max_edge)
            except ImageError as e:
                # Skipped, counted and named. Numbering does not skip
                # with it -- a gap in the sequence would show up as a
                # dangling link in the generated index.
                skipped.append(str(e))
                n -= 1
                continue
            out.append({"class": label, "name": _pretty(src), "path": rel,
                        "w": w, "h": h, "bytes": nbytes,
                        "src": os.path.basename(src)})
    return out


def write_indexes(images, outdir, pack):
    """Write maps.md / flags.md / images.md into the pack root.

    Named for their contents rather than sitting as index.md inside
    each directory, because these are the pages somebody goes looking
    for: "the flags" is a thing you want, "ark/arkmed/flags/index.md"
    is a path you have to already know.

    Links resolve relative to the file being shown (read.c:2480) and
    hand off to `view` by extension through ztype.h, so clicking one
    opens the image viewer with no further plumbing.
    """
    from . import cardfs

    by_class = {}
    for im in images:
        by_class.setdefault(im["class"], []).append(im)

    root = "%s/%s" % (cardfs.CARD_ROOT, pack)
    written = []
    for label in sorted(by_class):
        entries = sorted(by_class[label], key=lambda i: i["name"].lower())
        cardfs._check_83(label)
        out = ["# %s" % label.title(), "",
               "%d image%s. Links open in `view`."
               % (len(entries), "" if len(entries) == 1 else "s"), ""]
        for im in entries:
            rel = im["path"][len(root) + 1:]
            out.append("- [%s](%s)" % (cardfs._md_escape(im["name"]), rel))
        out.append("")
        name = "%s.md" % label
        full = os.path.join(outdir, root, name)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "wb") as fh:
            fh.write("\n".join(out).encode("utf-8"))
        written.append((name, len(entries)))
    return written
