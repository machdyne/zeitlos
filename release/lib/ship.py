#
# Zeitlos release tooling -- the ship half.
#
# Separate from the build half on purpose. Building is slow, uses the
# FPGA toolchain, and produces something you will want to program onto
# a real board and actually try before anybody else does. Publishing is
# a network operation that is hard to take back. Those are different
# decisions and they get different commands:
#
#   zrelease build v0.0.3      produces release/dist/v0.0.3/
#   ... flash it, boot it, look at it ...
#   zrelease ship v0.0.3       uploads what is in that directory
#
# `ship` never builds. If dist/<version>/ is not there, it says so
# rather than helpfully rebuilding, because a "helpful" rebuild is
# exactly how an unexamined image gets published.
#
# UPDATING AN EXISTING RELEASE is a first-class operation rather than a
# special case, because it is the common one: a release goes out, one
# target turns out to need a fix, and the other three are fine. `ship`
# on a tag that already exists uploads with --clobber, replacing assets
# by name and leaving anything it did not build alone. So
#
#   zrelease build v0.0.2 --targets lakritz_uart
#   zrelease ship  v0.0.2
#
# replaces that one target's assets on the existing v0.0.2 release and
# touches nothing else -- including the zeitlos.img.gz already there.
#

import json
import os
import shutil
import subprocess


class ShipError(Exception):
    pass


def preflight():
    if shutil.which("gh") is None:
        raise ShipError(
            "gh (the GitHub CLI) is not installed.\n"
            "  https://cli.github.com/ -- then `gh auth login`.")
    r = subprocess.run(["gh", "auth", "status"], capture_output=True,
                       text=True)
    if r.returncode != 0:
        raise ShipError("gh is not authenticated. Run `gh auth login`.\n%s"
                        % (r.stderr or r.stdout))


def _gh(args, cwd, dry=False, check=True):
    print("    $ gh %s" % " ".join(args))
    if dry:
        return ""
    r = subprocess.run(["gh"] + args, cwd=cwd, capture_output=True, text=True)
    if check and r.returncode != 0:
        raise ShipError("gh %s failed:\n%s%s"
                        % (" ".join(args), r.stdout, r.stderr))
    return r.stdout


def release_exists(tag, cwd):
    r = subprocess.run(["gh", "release", "view", tag, "--json", "tagName"],
                       cwd=cwd, capture_output=True, text=True)
    return r.returncode == 0


def existing_assets(tag, cwd):
    r = subprocess.run(["gh", "release", "view", tag, "--json", "assets"],
                       cwd=cwd, capture_output=True, text=True)
    if r.returncode != 0:
        return []
    try:
        return [a["name"] for a in json.loads(r.stdout).get("assets", [])]
    except ValueError:
        return []


def ship(root, version, distdir, dry=False, draft=False, prerelease=False,
         notes_only=False, title=None):
    preflight()

    tag = version if version.startswith("v") else "v" + version

    if not os.path.isdir(distdir):
        raise ShipError(
            "%s: not found.\n"
            "  Nothing has been built for %s. Run:\n"
            "    release/zrelease build %s"
            % (os.path.relpath(distdir, root), version, version))

    notes_path = os.path.join(distdir, "NOTES.md")
    if not os.path.exists(notes_path):
        raise ShipError("%s: no NOTES.md -- was the build interrupted?"
                        % os.path.relpath(distdir, root))

    assets = sorted(f for f in os.listdir(distdir)
                    if f not in ("NOTES.md",) and
                    os.path.isfile(os.path.join(distdir, f)))
    if not assets and not notes_only:
        raise ShipError("%s: no assets to upload" % distdir)

    exists = release_exists(tag, root)

    print("\n=== ship %s ===" % tag)
    print("    release %s" % ("exists -- updating" if exists else "is new"))

    if exists:
        # What is already up there, and what this run will replace.
        # Worth printing: --clobber is silent about it otherwise, and
        # "which of these did I just overwrite" is the question you ask
        # immediately afterwards.
        have = set(existing_assets(tag, root))
        replaced = [a for a in assets if a in have]
        added = [a for a in assets if a not in have]
        untouched = sorted(have - set(assets))
        for a in replaced:
            print("      replace  %s" % a)
        for a in added:
            print("      add      %s" % a)
        for a in untouched:
            print("      keep     %s  (not built by this run)" % a)

        _gh(["release", "edit", tag, "--notes-file", notes_path],
            root, dry=dry)
        if title:
            _gh(["release", "edit", tag, "--title", title], root, dry=dry)
        if not notes_only:
            _gh(["release", "upload", tag, "--clobber"]
                + [os.path.join(distdir, a) for a in assets],
                root, dry=dry)
    else:
        if notes_only:
            raise ShipError("release %s does not exist yet; --notes-only "
                            "has nothing to update" % tag)
        for a in assets:
            print("      add      %s" % a)
        args = ["release", "create", tag,
                "--title", title or ("Zeitlos %s" % version),
                "--notes-file", notes_path]
        if draft:
            args.append("--draft")
        if prerelease:
            args.append("--prerelease")
        args += [os.path.join(distdir, a) for a in assets]
        _gh(args, root, dry=dry)

    print("\n  https://github.com/machdyne/zeitlos/releases/tag/%s" % tag)
    return tag
