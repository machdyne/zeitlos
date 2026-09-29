# bbs on Linux

The BBS ([bbs.md](bbs.md)) as a server on a Linux machine: the same
core as on Zeitlos, the same data directory, more callers. This is how
`bbs.machdyne.com` is meant to run.

## Building

```
make -C sw/apps/bbs linux        # -> sw/apps/bbs/bbs-linux
```

A C99 compiler and a POSIX system; no libraries beyond libc. The SHA-256
is `sw/common/zsha256.c`, the same file the board uses.

## Running

```
bbs -d DATADIR [-t PORT] [-s SOCKET]         the server
bbs -d DATADIR [-s SOCKET] --connect         a caller, relayed (SSH's ForceCommand)
bbs -d DATADIR [-s SOCKET] --connect --local the sysop, on this machine
```

| option | default | |
|---|---|---|
| `-d DATADIR` | (required) | the data directory ([bbs.md](bbs.md#the-data-directory)); made if missing |
| `-t PORT` | 2323 | telnet; `0` for none |
| `-s SOCKET` | `DATADIR/bbs.sock` | where `--connect` finds the server |

One process, one `poll()` loop, as on Zeitlos. Log lines go to standard
error with a UTC timestamp -- to the journal under systemd. SIGTERM
stops it cleanly (everyone is disconnected, the socket removed).

### Telnet

The server speaks telnet itself, as netserve does on Zeitlos: it offers
ECHO and SGA, refuses every other option once (NAWS included -- the
screen size comes from detection), escapes 0xFF, and takes CR LF, CR NUL
or a bare LF as one Enter. IPv6 and IPv4 on one socket where the
machine has IPv6, IPv4 alone where it has not. **Telnet is not
encrypted**; the welcome screen says so to telnet callers.

### SSH, through OpenSSH

There is no SSH server in `bbs`: OpenSSH does SSH, and a `bbs` account
hands every session to the BBS. **For a server, use a second sshd for
the BBS alone** -- its own port, only the `bbs` account, the machine's
own sshd untouched: `sw/apps/bbs/linux/sshd_bbs_config` and
`sshd-bbs.service` -- **tested on Ubuntu 24.04's OpenSSH 9.6p1**: an
empty password into the BBS, `ssh bbs@host <command>` running the BBS
anyway, any other account refused, forwarding refused. The `bbs`
account's shell must be a real shell (`/bin/sh`): sshd runs the forced
command through it, and refuses an account whose shell does not exist.
The block below does the same inside the machine's own sshd, for a
machine where that is wanted instead.

```
useradd --system --create-home --home-dir /var/lib/bbs --shell /bin/sh bbs
passwd -d bbs                      # no password: see below
```

`/etc/ssh/sshd_config`:

```
Match User bbs
    ForceCommand /usr/local/bin/bbs -d /var/lib/bbs --connect
    PermitEmptyPasswords yes
    PasswordAuthentication yes
    KbdInteractiveAuthentication no
    PubkeyAuthentication no
    PermitTTY yes
    AllowTcpForwarding no
    AllowAgentForwarding no
    X11Forwarding no
    PermitTunnel no
    PermitUserRC no
```

Callers then `ssh bbs@bbs.example.com` and log in to the BBS itself --
the same login as telnet and as on Zeitlos, and the same decision
([bbs.md](bbs.md#decisions-so-far)): SSH provides the encryption, the
BBS the accounts. `ForceCommand` means the `bbs` account runs nothing
else, whatever the client asks.

**This block has not been tested against a real sshd** -- the dedicated
instance has (above). Check it with `sshd -t` and try a session from
outside before relying on it.
Things known to differ between systems: PAM can refuse an empty
password unless `pam_unix` has `nullok`; OpenSSH before 8.7 calls
`KbdInteractiveAuthentication` `ChallengeResponseAuthentication`.

`--connect` reads the caller's address from `SSH_CONNECTION`, puts the
terminal in raw mode, and relays it to the server's socket. The socket
is made readable only by its owner (mode 0600): the server believes
what a `--connect` client says about where its caller came from, so
only the `bbs` account may connect to it. Run the server as `bbs` too.

If the server is not running, `--connect` says so and exits.

### The sysop, locally

```
sudo -u bbs bbs -d /var/lib/bbs --connect --local
```

A local call: it logs in like any other, and shows as `local` in
who's online.

### systemd

`sw/apps/bbs/linux/bbs.service`:

```
install -m 755 sw/apps/bbs/bbs-linux /usr/local/bin/bbs
install -m 644 sw/apps/bbs/linux/bbs.service /etc/systemd/system/
cp -r sw/apps/bbs/data/. /var/lib/bbs/        # a first bbs.cfg and bulletin
chown -R bbs:bbs /var/lib/bbs
systemctl enable --now bbs
```

It runs as `bbs`, listens on port 23 with `CAP_NET_BIND_SERVICE` and no
other privilege, and can write only `/var/lib/bbs`. Then edit
`/var/lib/bbs/bbs.cfg` (at least `name`), restart, and **call it
yourself first**: the first account made is the sysop's.

## Moving between a card and a server

The data directory is the same format on both: copy `bbs.cfg`,
`users.dat`, `bulletins/` and `text/` either way. Passwords keep
working; the iteration count travels with each one. Keep the server's
`pw_iterations` in mind when moving users to a board: a hash that is
quick on a server blocks every caller on the board while it runs
([bbs.md](bbs.md#passwords)).

The Machdyne plan -- a Linux server as the public node and a Zeitlos
machine as a second one, with posts flowing between them -- is zfed
([fed.md](fed.md)): `fed` runs beside the BBS (`sw/apps/fed/linux/fed.service`,
as the `bbs` account -- its socket is readable only by its owner), and
`bbs.cfg`'s `fed: /var/lib/fed/fed.sock` points the BBS at it: without
that line, forums with a topic stay local ([bbs.md](bbs.md),
"Federation"). Its
network is managed with `sudo fed key`, `nodes`, `add`, `set` and
`remove` ([fed.md](fed.md), "Managing a network").
