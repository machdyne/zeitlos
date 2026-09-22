"""
Zeitlos
Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.

The results ledger: every score ever taken, appended as it is made, to
build/results.jsonl. Each record says exactly what was measured -- the
synthesiser's source (hashed), the pack (hashed), the material version
and hash, the recogniser -- so a number can always be traced back, a
cancelled run loses only what it had not yet written, and nothing is
measured twice.
"""

import glob
import hashlib
import json
import os
import time


def file_hash(path):
    if not path or not os.path.exists(path):
        return "none"
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()[:12]


def code_hash(repo):
    """The synthesiser and front end as built: every source file of sw/apps/tts."""
    h = hashlib.sha256()
    tts = os.path.join(repo, "sw", "apps", "tts")
    for p in sorted(glob.glob(os.path.join(tts, "*.[ch]")) +
                    glob.glob(os.path.join(tts, "tests", "tts_wav.c"))):
        h.update(os.path.basename(p).encode())
        h.update(open(p, "rb").read())
    return h.hexdigest()[:12]


class Ledger:

    def __init__(self, path):
        self.path = path
        os.makedirs(os.path.dirname(path), exist_ok=True)

    def key(self, rec):
        return (rec["code"], rec["pack"], rec.get("env", ""), rec["material"], rec["asr"], rec["set"])

    def find(self, **want):
        """The per-item results of an earlier identical measurement, or None."""
        if not os.path.exists(self.path):
            return None
        k = self.key(want)
        for line in open(self.path):
            try:
                rec = json.loads(line)
            except ValueError:
                continue
            # Only records scored the current way: items carry "ok".
            if self.key(rec) == k and rec.get("items") and "ok" in rec["items"][0]:
                return rec
        return None

    def add(self, rec):
        rec = dict(rec)
        rec["t"] = time.time()
        with open(self.path, "a") as f:
            f.write(json.dumps(rec) + "\n")
