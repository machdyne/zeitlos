"""
Zeitlos
Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.

Aligning a whole speech corpus: every clip's phones, placed in time.

    1. build the reference renderer (sw/apps/tts/tests/tts_wav.c, the
       same code the device runs, compiled for this machine);
    2. render every transcript with it, writing each phone's start
       and end beside the audio;
    3. warp each reference onto its recording (lib/aligner.py);
    4. write the recording's phones, a summary, and Audacity label
       files for a handful of clips so the result can be checked by
       eye and ear rather than taken on trust.

Everything lands in build/align/, which is gitignored like the rest of
build/.
"""

import multiprocessing
import os
import subprocess
import sys

from . import aligner


def read_metadata(corpus_dir, limit=0):
    """
    LJSpeech's metadata.csv: id|raw text|normalised text. The
    normalised column has numbers and abbreviations written out, which
    is what we want -- our own front end would do the same, and doing
    it twice risks disagreeing with the speaker about how "1884" was
    read.
    """

    meta = os.path.join(corpus_dir, "metadata.csv")
    wavs = os.path.join(corpus_dir, "wavs")
    out = []

    with open(meta, encoding="utf-8") as f:
        for line in f:
            parts = line.rstrip("\n").split("|")
            if len(parts) < 2:
                continue
            cid = parts[0]
            text = parts[2] if len(parts) > 2 and parts[2] else parts[1]
            wav = os.path.join(wavs, cid + ".wav")
            if not os.path.exists(wav):
                continue
            out.append((cid, text, wav))
            if limit and len(out) >= limit:
                break

    return out


def build_renderer(repo, out_bin):
    """Compiles the reference renderer for this machine."""

    os.makedirs(os.path.dirname(out_bin), exist_ok=True)
    tts = os.path.join(repo, "sw", "apps", "tts")
    srcs = ["tests/tts_wav.c", "phon.c", "synth.c", "text2ph.c", "lts.c", "pack.c", "dsyn.c"]
    # SYNTH_STATS: the renderer reports its overflow check, as `make wav` does.
    cmd = ["cc", "-std=gnu99", "-O2", "-w", "-DPACK_HOST", "-DSYNTH_STATS", "-I" + tts,
           "-I" + os.path.join(repo, "sw", "common"), "-o", out_bin]
    cmd += [os.path.join(tts, s) for s in srcs] + ["-lm"]
    subprocess.check_call(cmd)
    return out_bin


def _clean(text):
    # The testset format is '|'-separated, one line per utterance.
    return " ".join(text.replace("|", " ").split())


def render_references(renderer, clips, out_dir, pack, wpm=180, pitch=0, formants=0):
    """One reference wav and .phones per clip, in one run."""

    os.makedirs(out_dir, exist_ok=True)
    listing = os.path.join(out_dir, "refset.txt")
    with open(listing, "w", encoding="utf-8") as f:
        for cid, text, _ in clips:
            f.write("t_%s|%s|\n" % (cid, _clean(text)))

    env = dict(os.environ, ZTTS_MARKS="1")
    # Optionally in another voice: the acoustics calibration renders
    # at the SPEAKER's pitch and tract size, so the tracker's
    # pitch-dependent bias is the same on both sides.
    env.pop("ZTTS_PITCH", None)
    env.pop("ZTTS_FORMANTS", None)
    env.pop("ZTTS_VOICE", None)
    if pitch:
        env["ZTTS_PITCH"] = str(int(pitch))
    if formants:
        env["ZTTS_FORMANTS"] = str(int(formants))
    subprocess.check_call([renderer, listing, out_dir, str(wpm), pack or ""],
                          env=env, stdout=subprocess.DEVNULL)


def _one(job):
    cid, ref_dir, wav, out_dir = job
    ref_wav = os.path.join(ref_dir, "t_%s.wav" % cid)
    ref_ph = os.path.join(ref_dir, "t_%s.phones" % cid)
    if not os.path.exists(ref_ph):
        return cid, None, "no reference"
    try:
        got = aligner.align(ref_wav, ref_ph, wav)
    except Exception as e:                  # a bad clip must not stop the run
        return cid, None, str(e)
    if got is None:
        return cid, None, "did not align"
    with open(os.path.join(out_dir, cid + ".phones"), "w") as f:
        for name, a, b in got:
            f.write("%s %d %d\n" % (name, a, b))
    return cid, got, None


def align_all(clips, ref_dir, out_dir, jobs=0):
    os.makedirs(out_dir, exist_ok=True)
    work = [(cid, ref_dir, wav, out_dir) for cid, _, wav in clips]
    jobs = jobs or max(1, (os.cpu_count() or 2) - 1)
    results = {}
    failed = {}
    with multiprocessing.Pool(jobs) as pool:
        for i, (cid, got, err) in enumerate(pool.imap_unordered(_one, work, chunksize=8)):
            if got is None:
                failed[cid] = err
            else:
                results[cid] = got
            if sys.stdout.isatty() and (i + 1) % 25 == 0:
                sys.stdout.write("\r  %d of %d" % (i + 1, len(work)))
                sys.stdout.flush()
    if sys.stdout.isatty():
        # Clear the progress line, or the next thing printed lands on
        # top of it ("246s0 of 13100").
        sys.stdout.write("\r" + " " * 40 + "\r")
        sys.stdout.flush()
    return results, failed


def write_labels(results, out_dir, count=10):
    """
    Audacity label tracks for the first few clips: File > Import >
    Labels, next to the recording, shows every phone boundary we
    placed. The quickest way to see whether the alignment is right is
    to look at it.
    """
    os.makedirs(out_dir, exist_ok=True)
    made = []
    for cid in sorted(results)[:count]:
        path = os.path.join(out_dir, cid + ".txt")
        with open(path, "w") as f:
            for name, a, b in results[cid]:
                if name.startswith("_"):
                    continue
                f.write("%.3f\t%.3f\t%s\n" % (a / 1000.0, b / 1000.0, name))
        made.append(path)
    return made
