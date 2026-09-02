#!/usr/bin/env python3
#
# Zeitlos
# Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
#
# mkaudio -- prepare audio files for sw/apps/play.
#
# The player will happily open an ordinary WAV off the internet. This
# exists because "will open" and "will stream comfortably on this
# machine" are different questions, and the answer to the second is
# decided entirely at encode time.
#
# -- the constraint --
#
# sdmm.c moves one byte per MMIO write / poll / read cycle, roughly 48
# CPU cycles per byte (rtl/spisd.v's own header). SD bandwidth on this
# board is CPU, not waiting, and `play` gets about a third of the core
# with wm and a shell resident:
#
#   44.1kHz 16-bit stereo   176 KB/s   ~18% of a whole 48MHz core
#   22.05kHz 16-bit stereo   88 KB/s   ~9%
#   44.1kHz IMA ADPCM        44 KB/s   ~4.5%
#   22.05kHz IMA ADPCM       22 KB/s   ~2.2%
#
# The default is 8-bit PCM at 22.05kHz, and that is NOT chosen for
# bandwidth -- IMA ADPCM is four times smaller. It is chosen because
# rtl/audio_mixer.v fetches 8-bit samples from main memory as a bus
# master, so an 8-bit file plays with the CPU doing nothing per output
# frame at all. Measured on hardware, the software path spends about
# 1546 cycles per output frame against a budget of 358; the mixer path
# spends none of them.
#
# IMA remains available and remains the right choice if you are short
# of card space and can accept the software path -- but decoding it
# costs about 900 cycles per source frame, which on this machine is
# most of a CPU. See docs/play_app.md.
#
# -- why metadata is stripped --
#
# adec_parse() is shown the first 1KB of the file and walks the RIFF
# chunk list looking for `data`. A large LIST/INFO block -- which is
# what a tag editor leaves behind -- can push `data` past that window,
# and the player then refuses the file. Everything written here passes
# -map_metadata -1, so the question does not arise.
#
# -- 8.3 names --
#
# FatFs is built with FF_USE_LFN 0 (sw/os/fs/fatfs/ffconf.h), so the
# card has no long filenames. An output name longer than 8 characters
# is truncated here, visibly, rather than being silently mangled into
# something like MYFAVO~1.WAV by the filesystem later.
#
# Usage:
#   mkaudio.py song.flac                     -> SONG.WAV (IMA, 22.05k)
#   mkaudio.py -f wav16 -r 44100 song.flac
#   mkaudio.py --tone 440 -o TEST.WAV        (no ffmpeg needed)
#   mkaudio.py -d /media/sd/AUDIO *.mp3

import argparse
import math
import os
import shutil
import struct
import subprocess
import sys

FORMATS = {
    # name      ffmpeg codec         bytes/frame at 2ch  description
    "ima":   ("adpcm_ima_wav", 1.0, "IMA ADPCM -- NOT RECOMMENDED, see below"),
    "wav16": ("pcm_s16le",     4.0, "16-bit PCM -- best quality, 4x the bytes"),
    "wav8":  ("pcm_u8",        2.0, "8-bit PCM -- THE MIXER PATH; see docs/play_app.md"),
    "ulaw":  ("pcm_mulaw",     2.0, "u-law -- 8 bits, ~13-bit range, free to decode"),
    "alaw":  ("pcm_alaw",      2.0, "A-law -- as u-law, different companding"),
    "raw":   ("pcm_s16le",     4.0, "headerless 16-bit PCM (.RAW)"),
}

# IMA block size. Must be <= STREAM_MAX_BLOCK (sw/apps/play/stream.h)
# and must produce fewer frames per block than STREAM_SCRATCH_FRAMES.
# 512 is comfortably inside both: 1017 frames mono, 505 stereo.
IMA_BLOCK = 512


def die(msg):
    print("mkaudio: %s" % msg, file=sys.stderr)
    sys.exit(1)


def shortname(stem, ext):
    """8.3, uppercase, and loud about it when it has to cut."""
    clean = "".join(c for c in stem.upper()
                    if c.isalnum() or c in "_-")
    if not clean:
        clean = "TRACK"
    if len(clean) > 8:
        print("  name truncated: %s -> %s" % (clean, clean[:8]))
        clean = clean[:8]
    return clean + "." + ext.upper()


def have_ffmpeg():
    return shutil.which("ffmpeg") is not None


def convert(src, dst, fmt, rate, channels):

    codec, _bpf, _desc = FORMATS[fmt]

    cmd = ["ffmpeg", "-y", "-loglevel", "error", "-i", src,
           "-map_metadata", "-1", "-fflags", "+bitexact",
           "-ar", str(rate), "-ac", str(channels),
           "-acodec", codec]

    if fmt == "ima":
        cmd += ["-block_size", str(IMA_BLOCK)]
    if fmt == "raw":
        cmd += ["-f", "s16le"]

    cmd += [dst]

    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        die("ffmpeg failed on %s:\n%s" % (src, r.stderr.strip()))


def write_tone(path, hz, seconds, rate, channels, amp=0.5):
    """A test tone, in plain 16-bit WAV, with no external dependency.

    This is what to reach for on a board that has never made a sound:
    it removes ffmpeg, the encoder, the codec and the compression
    ratio from the list of things that could be wrong, leaving only
    the card, the player and the DAC.
    """
    n = int(rate * seconds)
    data = bytearray()
    for i in range(n):
        v = int(amp * 32767 * math.sin(2 * math.pi * hz * i / rate))
        for _ in range(channels):
            data += struct.pack("<h", v)

    hdr = b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt "
    hdr += struct.pack("<IHHIIHH", 16, 1, channels, rate,
                       rate * channels * 2, channels * 2, 16)
    hdr += b"data" + struct.pack("<I", len(data))

    with open(path, "wb") as f:
        f.write(hdr)
        f.write(data)


def main():

    ap = argparse.ArgumentParser(
        description="Prepare audio files for the Zeitlos play app.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="formats:\n" + "\n".join(
            "  %-7s %s" % (k, v[2]) for k, v in FORMATS.items()))

    ap.add_argument("inputs", nargs="*", help="source audio files")
    ap.add_argument("-f", "--format", default="wav8", choices=sorted(FORMATS),
                    help="output format (default: wav8, the hardware-mixer path)")
    ap.add_argument("-r", "--rate", type=int, default=22050,
                    help="sample rate (default: 22050)")
    ap.add_argument("-m", "--mono", action="store_true",
                    help="downmix to mono, halving the bandwidth again")
    ap.add_argument("-d", "--outdir", default=".",
                    help="output directory (default: .)")
    ap.add_argument("-o", "--output",
                    help="explicit output name, for a single input")
    ap.add_argument("--tone", type=float, metavar="HZ",
                    help="write a test tone instead of converting")
    ap.add_argument("--seconds", type=float, default=10.0,
                    help="length of --tone (default: 10)")

    args = ap.parse_args()
    channels = 1 if args.mono else 2

    os.makedirs(args.outdir, exist_ok=True)

    if args.tone is not None:
        name = args.output or shortname("TONE%d" % int(args.tone), "wav")
        path = os.path.join(args.outdir, name)
        write_tone(path, args.tone, args.seconds, args.rate, channels)
        print("  %s  %gHz tone, %gs, %d-bit stereo PCM"
              % (path, args.tone, args.seconds, 16))
        return

    if not args.inputs:
        ap.error("no inputs (or use --tone)")

    if not have_ffmpeg():
        die("ffmpeg not found. Install it, or use --tone for a test file.")

    if args.output and len(args.inputs) > 1:
        die("--output takes a single input")

    ext = "raw" if args.format == "raw" else "wav"

    # An --output whose extension disagrees with the format is worth
    # refusing rather than obeying. The player picks decoders by
    # extension first, so a headerless stream written as SONG.WAV is
    # parsed as RIFF, fails to find a header, and is reported as an
    # unrecognised file -- with nothing anywhere pointing at the
    # actual mistake.
    if args.output:
        got = os.path.splitext(args.output)[1].lstrip(".").lower()
        if got and got != ext:
            die("--output %s but format %s writes .%s"
                % (args.output, args.format, ext))
    _codec, bpf, _desc = FORMATS[args.format]
    bps = args.rate * bpf * (channels / 2.0)

    print("format %s, %dHz, %s -- about %.0f KB/s, %.1f%% of a 48MHz core"
          % (args.format, args.rate, "mono" if args.mono else "stereo",
             bps / 1024.0, (bps * 48.0) / 48e6 * 100.0))
    if bps > 180000:
        print("  WARNING: above ~176 KB/s this machine has no margin left.")
        print("           Expect `sv` to count on the player's status line.")

    if args.format == "ima":
        print("  WARNING: IMA cannot use the hardware mixer.")
        print("  Cost per sample delivered, measured on hardware:")
        print("      wav8   49 cyc of SD  +  37 cyc convert  =  86")
        print("      IMA    24 cyc of SD  + 450 cyc decode   = 474")
        print("  5.5x the CPU to save half the file size -- and the mixer")
        print("  truncates to 8 bits either way, so the audio is identical.")
        print("  Use -f wav8 unless you specifically want small files on a")
        print("  card and can accept playback at a fraction of real time.")
        print()

    for src in args.inputs:
        stem = os.path.splitext(os.path.basename(src))[0]
        name = args.output or shortname(stem, ext)
        dst = os.path.join(args.outdir, name)
        convert(src, dst, args.format, args.rate, channels)
        sz = os.path.getsize(dst)
        print("  %-16s %8.1f KB" % (name, sz / 1024.0))


if __name__ == "__main__":
    main()
