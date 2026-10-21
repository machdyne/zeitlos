# Distributions

A **distribution** is someone publishing Zeitlos releases for boards to
update from: a name, a place where the releases are, and the keys they
are signed with. **Machdyne Zeitlos** is the one this tree builds by
default, and the only one known so far. A fork that publishes its own
releases is another; so is anyone hosting a set of their own.

What `update` does with a release is in [update.md](update.md). This
page is about where releases come from and why a board can trust them.

## What a distribution is

Three public facts, committed in `release/dist.cfg`:

```
name = Machdyne Zeitlos
url = http://zeitlos.machdyne.com/release/
key = <64 hex digits>
```

- **name** is for people: `update` says where it is updating from.
- **url** is where boards fetch releases: the update store `zrelease`
  writes (objects named by hash, a manifest and its signature per
  release, `LATEST`), served as static files over plain HTTP.
- **key** is an Ed25519 public key. A board accepts a release whose
  manifest is signed by one of these keys, and nothing else. There may
  be several `key` lines (see "Changing the key").

`update` is built with them: `sw/apps/update/zdist.h` is made from
`dist.cfg` (`make -C sw/apps/update dist`) and committed, like
`sw/common/zabi.h`. The update build and `zrelease check` stop while it
is stale.

Two things are deliberately **not** in the repository, because the
repository is published: the **private key** that signs releases, and
**where `ship` uploads to**. Both are on the release machine only, in
`~/.zeitlos/` (or `$ZEITLOS_HOME`):

```
~/.zeitlos/sign.key       the private key: 64 hex digits, mode 600
~/.zeitlos/release.cfg    where ship uploads (below)
```

`zrelease` refuses a key file others can read, and one inside the
repository.

## Why plain HTTP is safe

Releases are fetched over plain HTTP, and the trust comes from the
signature, as Debian's apt has worked for decades:

1. `update` fetches the release's `manifest` and `manifest.sig`, and
   checks the signature (Monocypher's `crypto_ed25519_check`, RFC 8032)
   against the keys it trusts **before anything in the manifest is
   believed**.
2. The manifest names the release it is (`name v0.0.7`, signed), and
   `update` refuses one that is not the release it asked for: a server
   cannot pass off an older signed release as the one requested.
3. Every file is checked by **SHA-256** against the signed manifest
   before it is put in place. (Over TFTP a CRC is enough; it keeps out
   accidents, not people.)
4. Following `latest`, a release signed as **older** than the card's
   (its `made` time) is refused: a server going backwards may be serving
   an old release on purpose. Naming the older release, `update
   v0.0.6`, goes back to it; so does `-f`.

So a changed byte anywhere -- in transit, on a mirror, on a compromised
server -- stops the update or that file, and nothing unsigned reaches
the card. The private key is never on the server, so taking over the
server does not let anyone sign.

It also needs no clock, which TLS would (to check a certificate's
dates), and a new board with no card has none yet; and it costs `update`
about 14 KB of code where TLS, X.509 and the ciphers would be some 80 KB
and 33 KB of RAM (measured in `web`).

What it does not give:

- **Privacy.** Someone watching the network can see that a board is
  updating and which files it fetches. They are public files.
- **Freshness beyond "not backwards".** A server can withhold a newer
  release; it cannot substitute an older one for a newer name.
- **A leaked key** can sign anything. The fix is a new key, and boards
  that trust only the old one have to be told (below). This is true of
  any key a board trusts.

TFTP is for development on a LAN: `zrelease tftp` does not sign, and
`update -t tftp` does not ask for a signature.

## Making a distribution

On the release machine, once:

```
$ release/zrelease keygen --add      # ~/.zeitlos/sign.key, and its public key into release/dist.cfg
$ make -C sw/apps/update dist        # sw/apps/update/zdist.h from it
$ git commit release/dist.cfg sw/apps/update/zdist.h
```

A fork also changes `name` and `url` in `dist.cfg`. Nothing else in the
code names Machdyne.

Then, per release, `zrelease build` writes the update store beside the
images (`release/dist/update/release/`, one store for every version, as
the server has it), and `zrelease ship` signs it and uploads it.

## Publishing

`zrelease ship VERSION` does two things:

1. **GitHub**: the release and every asset, as before -- every image,
   `zeitlos-arkmedium.img.gz` included.
2. **The distribution's server**: the update store, signed, and the
   images people download (`zeitlos.img.gz`, `zeitlos-serious.img.gz`,
   `zeitlos-base.img.gz`; not arkmedium, which is on GitHub only).

`--no-server` does only the first, `--server-only` only the second.
`ship` refuses to sign with a key `dist.cfg` does not list, since every
board would refuse what it signs.

The server part is rsync, as `~/.zeitlos/release.cfg` says:

```
sign_key    = ~/.zeitlos/sign.key              (the default)
ship_store  = zeitlos@host:/srv/zeitlos/release/
ship_images = zeitlos@host:/srv/zeitlos/images/
ship_ssh    = ssh -i ~/.ssh/zeitlos_release    (optional)
```

in three passes: the objects first, then the manifests and their
signatures (`--delay-updates`, so each release's two appear together),
`LATEST` last -- a board never finds a manifest whose files are not
there yet. Nothing on the server is deleted: old releases stay, so
boards can still go back to them. Images go to `ship_images/vVERSION/`.

### The server

Static files, nothing else: no CGI, no PHP, nothing from the repository
runs there. The `zeitlos` account should be able to do one thing, write
into its directory with rsync, and not have a shell. With OpenSSH's
`rrsync` (shipped with rsync), its `~/.ssh/authorized_keys` holds the
release machine's public key as:

```
command="rrsync /srv/zeitlos",restrict ssh-ed25519 AAAA... release machine
```

and the paths in `release.cfg` are then relative to `/srv/zeitlos`.

Served over **plain HTTP**. A site that redirects everything to HTTPS
needs an exception for the release path, ahead of the redirect -- with
nginx, for example:

```
server {
    listen 80;
    server_name zeitlos.machdyne.com;
    location ^~ /release/ {
        alias /srv/zeitlos/release/;
        autoindex off;
    }
    location ^~ /images/ {
        alias /srv/zeitlos/images/;
    }
}
```

A host name of its own (`zeitlos.machdyne.com`) keeps it apart from
anything else on the web server, and lets it move to another server by
changing DNS. `update` says plainly when a server answers with a
redirect to `https://`: it cannot follow it, and should not need to.

Any static web server works -- keep-alive and `Content-Length` (or
chunks) are all `update` uses -- including one on a LAN:

```
$ release/zrelease http --dest /srv/www/zeitlos/release    # signed, like ship's
> update v0.0.7 -t http http://192.168.1.10/zeitlos/release/
```

## Following another distribution

A board follows the distribution its `update` was built with, unless
`/sys/zeitlos.cfg` on its card says otherwise ([config.md](config.md)):

```
system.update.url: http://example.org/zeitlos/release/
system.update.key: <their public key, 64 hex digits>
```

The URL alone is a **mirror** of the built-in distribution: same keys,
another server. URL and key are **another distribution**. Since
`update` needs a card anyway, and `zeitlos.cfg` is a file an update
keeps once it has been edited, this survives updates and goes with the
card. `update -t http URL` uses a URL once, with the same keys.

## Changing the key

A board knows the keys its `update` was built with (and the one in its
`zeitlos.cfg`). So a distribution changes keys in two steps:

1. Add the new key to `dist.cfg` and sign with **both**: `manifest.sig`
   has one line per key, and a board accepts any line by a key it
   trusts. New flashes carry both keys; old ones keep working.
2. Much later, stop signing with the old one. A board still on the old
   flash then refuses, and says why: reflash it, or put the new key in
   `system.update.key` (`system.update.key2` holds a second one).

Keys need changing rarely: an Ed25519 key that never leaves the release
machine can last indefinitely. The reason to retire one is that it
leaked, and then step 1 cannot help -- whoever has it can sign too.

## Known distributions

| name | URL | |
|---|---|---|
| Machdyne Zeitlos | `http://zeitlos.machdyne.com/release/` | this tree's default |

## Files

| | |
|---|---|
| `release/dist.cfg` | the distribution: name, URL, public keys |
| `release/lib/dist.py` | reads it, writes `zdist.h`, keygen, the release machine's config |
| `release/lib/ed25519.py` | signatures, in plain Python (RFC 8032) |
| `release/lib/store.py` | `sign()`, `check_sig()`: `manifest.sig` |
| `sw/apps/update/zdist.h` | the distribution as `update` has it (generated) |
| `sw/apps/update/upd.c` | `check_sig()`: what the board checks |
| `sw/apps/update/upd_net.c` | the HTTP client |
