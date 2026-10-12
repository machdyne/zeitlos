# NFS

A directory on a PC, read-only at `/nfs`. `net` mounts an NFS export
from the server, and `fs` serves it beside the card, `/ram` and `/usb`:
`ls /nfs`, `text /nfs/notes.txt`, and any app that opens a file can
read from it. For a machine with no card it is a way to more files than
fit in flash, and for development it is the end of `tget` after every
build: put the builds in the export's `apps/` and the machine runs the
latest one by its bare name.

```
> nfs mount 192.168.1.100:/srv/nfs/zeitlos
nfs: mounted at /nfs
> ls /nfs/apps
> run text            /nfs/apps/text, the build just made on the PC
```

## Status (October 2026)

Written and tested on the host, end to end: fs and `net`'s real client
(`sw/apps/net/nfs.c`) against a scripted NFSv3 server over a fake TCP
that delivers in pieces (`sw/apps/fs/tests/nfs_fake.h`, in `test_fs`).
**On hardware:** mounted from a Linux `nfsd` over a USB ethernet
adapter, and a program run from `/nfs/apps` by its bare name. Not yet
measured: read throughput, a server going away mid-read.

| | |
|---|---|
| Protocol | NFSv3 over TCP, from a port below 1024 (no `insecure` needed) |
| Access | read-only: writes, `mkdir`, `rm`, `rename` on `/nfs` are `EROFS` |
| Mounting | `nfs mount <address>:/export` at the console, or `nfs=` in `net.cfg` at boot |
| Programs | `/nfs/apps/<name>` is looked in first, before the card and flash (`system.apps.nfs_first`, on by default) |
| Cost | in `net`: about 8KB of code and 6KB of buffers; `net` paid for it ([flash_apps.md](flash_apps.md)) |

## Using it

### On the PC

Any NFS server that speaks v3 over TCP. On Linux, with
`nfs-kernel-server` installed, one line in `/etc/exports`:

```
/srv/nfs/zeitlos  192.168.1.0/24(ro,all_squash,no_subtree_check)
```

then `sudo exportfs -ra`. `ro` because nothing is ever written;
`all_squash` because the client asks as root (uid 0) and there is no
reason to let it be anyone. The default `secure` is fine: `net` asks
from a port below 1024. The files must be readable by the squashed user
(`nobody`), so `chmod -R a+rX` the export.

`rpcbind` must be running (port 111): `net` asks it where MOUNT and
NFS are, as every v3 client does.

### Mounting

At the console:

```
nfs                                   is anything mounted?
nfs mount 192.168.1.100:/srv/nfs/zeitlos
nfs umount
```

The server is given as an address -- there is no name lookup. A mount
is three conversations with the server (rpcbind, MOUNT, then NFS), five
seconds each at most; `net` prints what failed if one does. Mounting
another export replaces the first.

At every boot, from `/data/net/net.cfg`:

```
nfs=192.168.1.100:/srv/nfs/zeitlos
```

`net` mounts it in the background once the network is up; nobody waits
for it, and the desktop comes up either way. If the mount fails -- the
server is off, the link not up yet -- it is tried again every 30
seconds until it is made, or until `nfs mount` or `nfs umount` at the
console takes over. `/nfs` appears at the root
listing when the mount is made.

### Running programs from it

With an export mounted, a bare name -- a dock icon, `run text`, boot
starting `wm` -- is looked for in `/nfs/apps/<name>` first, then the
card's `/apps`, then flash ([layout.md](layout.md#finding-a-program)).
Not for the drivers `fs`, `usb` and `net`: the first two are what
`/nfs` is reached through and `net` is the client itself, so they never
come from it.

`system.apps.nfs_first = no` in `/sys/zeitlos.cfg` puts `/nfs/apps`
back out of the search ([config.md](config.md)); a full path
(`run /nfs/apps/text`) still works.

To fill the export from a build tree:

```
$ make apps && make nfs-dist NFS_DIR=/srv/nfs/zeitlos
```

copies every built `sw/apps/<name>/<name>.bin` to
`$(NFS_DIR)/apps/<name>`, each renamed into place, so a machine reading
one at that moment sees the old binary or the new one, never half of
each. Then start the app again on the machine.

## Limits

- **Read-only.** By design: what fetching programs and files needs, and
  nothing on the PC can be damaged from here.
- **Names are the server's**: case-sensitive, unlike the rest of the
  filesystem (FAT). `/nfs/apps/Text` is not `/nfs/apps/text`.
- **Symbolic links** are listed as files but cannot be read; NFS hands
  the link itself to the client to follow, and this one does not. Copy
  files into the export rather than linking them.
- **Sizes past 4GB** show as 4GB; such a file cannot be read past it.
- **One request at a time.** `fs` asks `net` and waits, as it waits for
  the card; a read is 4KB at a time.
- **A server that stops answering** costs each request five seconds
  (fs gives up after six) before it fails with a timeout; a dropped
  connection is opened again by the next request.
- **`fs` waiting on `net` while `net` waits on `fs`** -- `net` saving a
  `tget` to the card at the moment `fs` is asking for an NFS read --
  stalls both until `fs` gives up on the read, six seconds later; then
  the save goes on and the read fails. It cannot happen while nothing is
  mounted.
- **No locking, no caching of file data.** Each read goes to the
  server; a file changed on the PC is seen at the next read, which is
  what development wants.

## How it works

### The pieces

```
app --zfsc--> fs --Z_NET_NFS_REQ--> net --NFSv3/TCP--> server
```

- `sw/common/znfsproto.h` -- what fs and the kernel shell ask `net`
  (`Z_NET_NFS_REQ`, `_REPLY`, `_EVENT`): LOOKUP a path, READ from a file
  handle, READDIR a page, MOUNT, UMOUNT, STATUS.
- `sw/apps/net/nfs.c` -- the client: RPC with record marking (RFC 5531),
  AUTH_SYS as uid 0, XDR by hand.
- `sw/apps/fs/fs.c` -- `/nfs` as a volume: open, read, stat and list
  through `net`; everything that writes is `EROFS`.
- `sw/os/kfs.c` -- `/nfs/apps` first in the search, while
  `system.apps.nfs_first` allows.
- `sw/os/sh.c` -- the `nfs` command.

### State lives in fs, not net

`net` keeps nothing per file. fs holds the server's file handles (up to
64 bytes) in its open files and directories and asks with them, so an
fs restart, or a program that dies with a file open, leaves nothing in
`net`. The one thing `net` remembers besides the mount is one directory
and its handle: opening `/nfs/apps/text` after `/nfs/apps/term` is one
LOOKUP, not a walk from the root.

`net` tells fs when a mount comes up or goes (`Z_NET_NFS_EVENT`). While
nothing is mounted, fs answers a `/nfs` path at once, without asking.

### Buffers

A reply is assembled whole in one 4.6KB buffer, then turned into the
answer to fs in the same place: the answer's header in front, the data
moved down to meet it -- a READ's bytes, or a READDIRPLUS listing
converted to fs's `z_fs_dirent_t` records, which only ever get shorter.
Requests are built in a 512-byte buffer. No copy of a whole reply is
made, and no buffer of `net`'s TCP or sockets was trimmed for it.

### Paying for it

The client cost `net` about 8KB of flash and 14KB of RAM. It came from
building `net` with `ZFMT_LITE` -- stdout without newlib's stdio, about
8KB of each ([build.md](build.md#integer-only-printf-zfmt)) -- and from
`text`'s paste buffer moving to the stack, which let `text`'s RAM
budget come down: `net` 144K/285K, `text` 86K/154K
([flash_apps.md](flash_apps.md#how-much-fits)). The archive and the RAM
rule (1,022 of 1,024 KB, with `wm`'s reboot dialog since) still hold.

## Testing

- **Host:** `make -C sw/apps/fs/tests && sw/apps/fs/tests/test_fs`
  (and `test_fs_kernel`). `test_nfs()` mounts through the kernel shell's
  request, then opens, reads (across 4KB requests and short replies),
  seeks, stats, lists a 70-entry directory a page at a time, checks
  `EROFS` for every write, a missing name, a file with no attributes in
  its LOOKUP reply, a server that stops answering (timeout and
  recovery), connections from a port below 1024, unmounting, and
  `net.cfg`'s mount being tried again every 30 s until it is made.
- **Hardware:** a Linux PC with the export above, then `nfs mount`,
  `ls /nfs`, `text /nfs/<file>`, `make nfs-dist`, and `run <app>` with
  the new build; `nfs=` in `net.cfg` and a reboot; `nfs umount`.

## Implementation log

**October 2026.** Written after the network rounds 4 and 5 made TCP
fast enough to load programs over it. In `net` rather than as an app of
its own, so a machine with no card has it from flash; NFSv3 over TCP
because current Linux servers have v2 and UDP off by default. Host
tests found two bugs before hardware: the LOOKUP path sent without
opening the connection first (every lookup timed out), and a forward
declaration fs needed. To fit the core-app budgets, `net` gained
`ZFMT_LITE` (8KB of flash and RAM back) and `text`'s 4KB paste buffer
left `.bss`; `kfs.c` keeps the three drivers out of the `/nfs/apps`
search.

The first try on hardware never reached the server: the bitstream had
the ESP32 link, so `net` chose it, but the Kabellos PMOD was not
plugged in and a USB ethernet adapter was. `net` now falls back to the
adapter when the ESP32 says nothing ([usb_ethernet.md](usb_ethernet.md#configuration)),
and `net.cfg`'s mount is retried every 30 s instead of being given up
after one try.
