#!/usr/bin/env python3
#
# Zeitlos release tooling -- the distribution (docs/distribution.md).
#
# Two configurations, kept apart on purpose:
#
#   release/dist.cfg              committed, public: the distribution's
#                                 name, update URL and public keys
#   ~/.zeitlos/release.cfg        the release machine's, never in the
#   ~/.zeitlos/sign.key           repo: where `ship` uploads, and the
#                                 private signing key
#
# ($ZEITLOS_HOME replaces ~/.zeitlos.) Nothing in the repository says
# how to reach the server or holds a secret, since the repository is
# published.
#
# The board's copy of the public facts is sw/apps/update/zdist.h,
# generated from dist.cfg and committed, like sw/common/zabi.h:
#
#   python3 release/lib/dist.py --header sw/apps/update/zdist.h
#   python3 release/lib/dist.py --check  sw/apps/update/zdist.h
#
# release.cfg, `key = value` lines:
#
#   sign_key    = ~/.zeitlos/sign.key      the private key (default)
#   ship_store  = zeitlos@host:/srv/zeitlos/release/
#                                          rsync destination of the store
#   ship_images = zeitlos@host:/srv/zeitlos/images/
#                                          rsync destination of the images
#   ship_ssh    = ssh -i ~/.ssh/zeitlos_release   (optional: rsync -e)

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
DIST_CFG = os.path.join(ROOT, "release", "dist.cfg")
HEADER = os.path.join("sw", "apps", "update", "zdist.h")
KEY_RE = re.compile(r"^[0-9a-f]{64}$")
URL_RE = re.compile(r"^http://[A-Za-z0-9.-]+(:[0-9]{1,5})?/([A-Za-z0-9._~/-]*/)?$")


class DistError(Exception):
    pass


def _read_kv(path):
    out = []
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            k, sep, v = line.partition("=")
            if not sep:
                raise DistError("%s:%d: not `key = value`" % (path, n))
            out.append((k.strip(), v.strip()))
    return out


class Dist(object):
    def __init__(self):
        self.name = ""
        self.url = ""
        self.keys = []          # hex public keys


def load(path=DIST_CFG):
    d = Dist()
    for k, v in _read_kv(path):
        if k == "name":
            d.name = v
        elif k == "url":
            d.url = v
        elif k == "key":
            v = v.lower()
            if not KEY_RE.match(v):
                raise DistError("%s: key %r is not 64 hex digits" % (path, v))
            if v not in d.keys:
                d.keys.append(v)
        else:
            raise DistError("%s: unknown setting %r" % (path, k))
    if not d.name or '"' in d.name or "\\" in d.name or len(d.name) > 48:
        raise DistError("%s: name must be set (48 characters at most, no "
                        "quotes)" % path)
    if not URL_RE.match(d.url) or len(d.url) > 200:
        raise DistError("%s: url %r must be http://host[:port]/path/ "
                        "(plain HTTP: boards check signatures, not TLS)"
                        % (path, d.url))
    return d


def header_text(d):
    keys = " ".join(d.keys)
    return """#ifndef ZDIST_H
#define ZDIST_H

/*
 * zdist.h -- GENERATED from release/dist.cfg by release/lib/dist.py
 * (`make -C sw/apps/update dist`). Do not edit.
 *
 * The distribution `update` follows when zeitlos.cfg does not name
 * another (system.update.url, system.update.key): its name, where its
 * releases are, and the Ed25519 public keys whose signatures it
 * accepts, as hex, separated by spaces. docs/distribution.md.
 */

#define Z_DIST_NAME "%s"
#define Z_DIST_URL  "%s"
#define Z_DIST_KEYS "%s"

#endif
""" % (d.name, d.url, keys)


# -- the release machine's own --

def home():
    return os.environ.get("ZEITLOS_HOME") or os.path.expanduser("~/.zeitlos")


def local_cfg():
    """{key: value} from release.cfg, {} if there is none."""
    p = os.path.join(home(), "release.cfg")
    if not os.path.isfile(p):
        return {}
    out = {}
    for k, v in _read_kv(p):
        if k not in ("sign_key", "ship_store", "ship_images", "ship_ssh"):
            raise DistError("%s: unknown setting %r" % (p, k))
        out[k] = v
    return out


def key_path():
    p = local_cfg().get("sign_key") or os.path.join(home(), "sign.key")
    return os.path.expanduser(p)


def load_seed(path=None):
    """The private key (32 bytes). DistError if missing or readable by
    others."""
    path = path or key_path()
    if not os.path.isfile(path):
        raise DistError("no signing key at %s (zrelease keygen makes one)"
                        % path)
    st = os.stat(path)
    if st.st_mode & 0o077:
        raise DistError("%s is readable by others (chmod 600 it)" % path)
    with open(path, encoding="utf-8") as f:
        text = f.read().strip()
    if not KEY_RE.match(text):
        raise DistError("%s: not a key (64 hex digits)" % path)
    seed = bytes.fromhex(text)
    if os.path.realpath(path).startswith(os.path.realpath(ROOT) + os.sep):
        raise DistError("%s is inside the repository: keep the private "
                        "key outside it" % path)
    return seed


def keygen(path=None, force=False):
    """Makes a private key at `path`. Returns the public key, hex."""
    import ed25519
    path = path or key_path()
    if os.path.exists(path) and not force:
        raise DistError("%s already exists (--force replaces it, and "
                        "boards that trust only its key will refuse "
                        "releases signed with the new one)" % path)
    if os.path.realpath(os.path.dirname(os.path.abspath(path))).startswith(
            os.path.realpath(ROOT)):
        raise DistError("%s is inside the repository: keep the private "
                        "key outside it" % path)
    os.makedirs(os.path.dirname(os.path.abspath(path)), mode=0o700,
                exist_ok=True)
    seed = os.urandom(32)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f:
        f.write(seed.hex() + "\n")
    os.chmod(path, 0o600)
    return ed25519.public_key(seed).hex()


def add_key(pub_hex, path=DIST_CFG):
    d = load(path)
    if pub_hex in d.keys:
        return False
    with open(path, "a", encoding="utf-8") as f:
        f.write("key = %s\n" % pub_hex)
    return True


def main(argv):
    if len(argv) == 3 and argv[1] in ("--header", "--check"):
        try:
            text = header_text(load())
        except (DistError, OSError) as e:
            sys.stderr.write("dist: %s\n" % e)
            return 1
        out = argv[2]
        try:
            with open(out, encoding="utf-8") as f:
                same = f.read() == text
        except OSError:
            same = False
        if argv[1] == "--check":
            if same:
                return 0
            sys.stderr.write("dist: %s is stale: release/dist.cfg changed; run "
                             "`make -C sw/apps/update dist` and commit it\n" % out)
            return 1
        if not same:
            with open(out, "w", encoding="utf-8") as f:
                f.write(text)
        print("dist: %s %s" % (out, "unchanged" if same else "written"))
        return 0
    sys.stderr.write("usage: dist.py --header|--check OUT\n")
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
