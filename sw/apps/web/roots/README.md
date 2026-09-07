# The trust store

`roots.der` on the card is what `sw/apps/web` believes about the
world. Everything HTTPS rests on it.

## Where these certificates come from

**They are not generated.** CA roots are other organisations'
certificates, and all a distribution does is choose which of them to
trust. Every mainstream list traces back to **Mozilla's CA
Certificate Program** (the NSS store); Debian and Ubuntu's
`ca-certificates` package is that list, repackaged.

The input to vendor here is curl's distribution of it:

    https://curl.se/ca/cacert.pem

Mozilla-derived, dated, and published with a SHA-256 alongside it.

`/etc/ssl/certs/ca-certificates.crt` on a build machine is the same
list at whatever vintage that machine happens to have. It is fine for
a local build and is the wrong thing to ship, because nobody can tell
afterwards what was in it.

## Why it is vendored and not downloaded

**The trust store is the trust anchor.** A build that fetches it can
silently change what every Zeitlos machine believes, and a
compromised download is a compromised device — with no signature to
check afterwards, because this file is the thing signatures are
checked against.

So the PEM lives here, its SHA-256 is recorded in the Makefile, and
`tools/mkroots.py --expect-sha256` turns a changed input into a build
failure rather than a surprise. Updating it is then a visible commit
that a person reviews, which is the only review it will ever get.

## Size, and choosing a subset

The full Mozilla set is about 150 certificates and 160KB. That is
fine on a 32MB board and it is not fine everywhere, so a subset is a
legitimate choice: twenty roots cover the large majority of public
web traffic.

A subset is a **trust decision, not a size optimisation**, and it cuts
both ways:

- Fewer roots means fewer CAs that can issue a certificate this
  machine will believe. That is a security improvement.
- It also means sites chaining to an omitted root simply do not work,
  with an error that says the chain is untrusted rather than that the
  store is small.

There is a performance consequence too, and it is not small. `web`
stops walking a chain at the first certificate whose issuer is in the
store (see `docs/x509.md`), so **including a widely-used intermediate
turns a three-signature chain into one**. On hardware that is the
difference between an RSA-rooted site loading and the server hanging
up mid-verification.

## The format

`tools/mkroots.py` writes an INDEX in front of the certificates: a
sorted table of (subject-DN hash, offset, length). `web` reads a few
kilobytes and seeks, rather than parsing every certificate to find
one.

Measured on hardware, the scan took **1.65 seconds** on the first
HTTPS fetch of every run. It is build-time work and it now happens at
build time.

A plain concatenation of DER still works — `web` falls back to
scanning — so an existing card keeps working, just slower.

## Building it

    python3 tools/mkroots.py roots/cacert.pem -o roots.der

and copy `roots.der` to `/web/roots.der` on the card. `make roots` in
`sw/apps/web` does both.
