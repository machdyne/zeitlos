# Backup

Kind: a **desktop app** ([app_kinds.md](app_kinds.md)).

Dated backups of your files on a second drive -- a USB stick, or an SD
card in a USB reader -- and getting any file back, as it was at any of
those times. It is not a mirror: a file you delete or spoil on the card
stays as it was in every earlier backup.

Once it is set up you press one button (or type `backup`) at the end of
a session, and if the card fails while you are writing a book in `text`,
the book is on the stick, at every time you backed it up. With **watch**
on, and the window open in a corner, every save of the book is on the
stick 10 seconds later.

## Status (October 2026)

| | |
|---|---|
| **Done** (Phase 0) | the format on the drive, and `sw/apps/backup/bk.c`, the engine: backing up, making room, restoring, history, checking, pins. Tested on the host, including a power cut at every single write ("Testing"). `tools/backup_read.py` reads a backup drive on any PC. |
| **Done** (Phase 1) | `backup` on the board (`sw/apps/backup`, in the `base` set): the commands below from posix and the kernel shell, the settings in `zeitlos.cfg`, eject. Its file layer (`bk_zfs.c`) tested on the host against the real fs driver and FatFs on a FAT32 image, with the power cut at every sector write ("Testing"). **Not yet run on hardware.** |
| **Done** (Phase 2) | the window (since redone, below): one button, a restore browser, a small view, settings; the engine run a slice at a time in a coroutine, one request to fs a slice, stoppable at once (tested on the real fs driver). Listing and status read only each index's last line. **Not yet run on hardware.** |
| **Done** (Phase 3) | watch ("Watch"): a file saved in a watched folder is backed up on its own 10 seconds later, while the window (or `backup watch`) is open, through change notices from fs to the programs that ask for them. A dock icon. Tested on the host against the real fs driver. **Not yet run on hardware.** |
| **Done** (window, redone) | one small window: a headline, Back up now, Show backups (the drive in `files`), Settings (two lines), Remove drive; the Aa icon for a strip. No restore browser: files are copied back, or `backup restore` ("The window"). **Not yet run on hardware.** |

The design and the decisions behind it are recorded at the end
("Decisions").

## Using it

From the desktop, the window ("The window"): its dock icon in the
Tools group (three dated copies and a clock; `sw/data/wm.cfg`,
[dock.md](dock.md)), `backup window` in posix, or `run backup` at
the kernel console.
In posix (or `run backup now` / `run backup ...` at the kernel console):

```
backup                       back up now (in posix; at the console, the window)
backup now                   back up now, wherever it is typed
backup window                the window
backup -n                    what would be copied and removed; writes nothing
backup -e                    back up, then eject the drive
backup status                the drive, the newest backup, what is backed up
backup list                  the backups, oldest first
backup files [BACKUP] [DIR]  what a backup had
backup history PATH          every copy of one file
backup restore PATH [BACKUP] [-o OUT] [--replace]
backup check [-a]            every index; -a reads every copy back
backup pin BACKUP, backup unpin BACKUP
backup make-room KB
backup eject                 release the drive; it can then be pulled
backup watch [PATH...]       back up each save there 10 s after, until stopped ("Watch")
```

`BACKUP` is a backup's name or the start of one: `2026-10-21` is the
newest backup of that day. Left out, the newest.

A run looks like this:

```
$ backup
backup: 214 files on the card: 3 to copy (812 KB), 211 unchanged, 1 gone since the last backup
backup: to /usb/backup/zeitlos/2026-10-21_18-02-31
  /home/book.txt
  /home/pics/cat.bmp
  /data/sheet/budget.zss
backup: done: 2026-10-21_18-02-31, 213 files, 3 copied (812 KB)
$ backup history /home/book.txt
  2026-10-19_18-00-12             1834 bytes  2026-10-19 18:00:12
  2026-10-21_18-02-31             2410 bytes  2026-10-21 18:02:31
$ backup restore /home/book.txt 2026-10-19
  /home/book (2026-10-19 18-00).txt
backup: 1 of 1 restored from 2026-10-19_18-00-12
```

(The numbers are an illustration.) A file of 256 KB or more shows its
progress on one line while it is copied.

**Where the output goes.** Typed in posix, in that terminal; from the
kernel console, on the console -- as `update` does
([update.md](update.md)).

**Stopping it.** `kill backup0` in posix, `kill <pid>` at the console,
or from `info` (Ctrl+C does not reach a program posix started,
[posix.md](posix.md#ctrlc)). It is safe at any moment: a backup stopped
half way has no index yet, nothing visible has changed, and the next
backup removes what it left.

**Exit status:** 0 done (or `-n`, or nothing to do); 1 done, but a file
was left out (unreadable, or its path too long) or changed while it
was copied, or `check` found a problem; 2 nothing done (no drive, no
room, another backup running, ...); 3 a usage error.

**Its arguments** arrive as the launch argument, at most 96 characters
(`Z_WM_ARG_MAX`): a very long `restore ... -o ...` line has to be
shorter.

**Memory.** It holds the newest backup's index and the card's file
list, about 100 bytes a file, and asks for 512 KB (`APP_STACK`): enough
for some 2,500 files. Past that it says "out of memory" and writes
nothing. A 1 MB board cannot start it beside the boot set.

A restored file goes **beside** the one on the card, named after the
backup -- `book (2026-10-21 11-30).txt` -- unless the two are the same
(then nothing is done) or `--replace` is given. It is written as
`NAME.part`, read back, and renamed, so a power cut leaves the old file
or the new one, never half of one.

## The window

```
+- Backup: today 14:40, watching -------------- Aa x -+
|  Backed up today 14:40                              |
|  412 files - /usb, 12.3 GB free                     |
|  Watching /home/book: ch3.txt backed up at 14:38    |
|                                                     |
|  [Back up now] [Show backups] [Settings] [Remove drive]
+-----------------------------------------------------+
```

It answers one question -- am I backed up? -- in its headline, with a
line under it (how many files, the drive, its free space) and, with
watch on, a line for the watch ("Watch"). **Back up now** (Enter) is the
one button that matters. Only the buttons that can be pressed are there:

| state | headline | buttons |
|---|---|---|
| backed up | Backed up today 14:40 | Back up now, Show backups, Settings, Remove drive |
| a new drive | No backups yet | Back up now, Settings, Remove drive |
| backing up | Backing up... (and what, a bar, "Keep the power on") | Stop (Esc) |
| no drive | Plug in the backup drive | Settings |
| after Remove drive | You can pull out the drive now | Settings |
| it failed | Not backed up (and why, the engine's last line) | as backed up |
| no clock | Last backup: no. 12 | as backed up |

A watch backup does not take the window over: the watch's line says
"Backing up ch3.txt now", and the rest stays as it was. Back up now
pressed during one (or while the drive is being read) starts right
after it.

**Show backups** (B) opens the backup folder in `files`:
`README.TXT` there says how to get files back, beside this computer's
folder of backups. Each backup is a folder named by its date, holding
only the files that had changed then, so **the newest copy of a file
is in the newest folder that has it**: copy it back. A whole folder,
or a file without looking: `backup restore /home` (or `PATH`) in a
shell, which puts it beside what is on the card. The window has no
restore of its own (decided 2026-10-21: copying is something people
already know; a browser with its own rules, used rarely and under
stress, is not).

**Remove drive** releases a USB drive so it can be pulled out.

**Settings** (S) has two lines: **What to back up**
(`apps.backup.from`) and **Back up each save** (`apps.backup.watch`,
empty for off). The rest -- the folder on the drive, Skip, Keep, eject
after each backup -- is in `zeitlos.cfg` ("Settings").

**The Aa icon** in the titlebar (or M) makes the window a strip for a
corner of the screen -- the headline, the watch's line, Back up -- and
back. **Shaded** (Alt+double-click the titlebar,
[window_manager.md](window_manager.md)) the title alone says it:
`Backup: today 14:40`, `Backup: today 14:40, watching`, `Backup 45%`,
`Backup: ch3.txt saved`, `Backup: Plug in the backup drive`.

Keys: Enter back up (or the focused button), Esc stop or back, B, S, M
as above, F5 look at the drive again, Tab / arrows between buttons, F1
help. When a button goes (the drive is pulled), the focus goes with it
rather than to the next button, so Enter never opens something nobody
chose; Back up now takes it again when the drive is back.

**It answers at once.** Everything on the drive runs a slice at a time
in a coroutine, one request to fs a slice ([responsive.md](responsive.md#backup)),
and Esc stops it within a few slices. While idle the window asks once a
second whether the drive is there (one quick request), and reads the
drive again when it comes. Two things can still make it wait: the first
free-space count on a stick whose FSINFO is not valid ("Running
alongside everything"), and a dialog (a prompt, the help box), during
which a running job pauses.

## Settings

In `/sys/zeitlos.cfg` ([config.md](config.md)); the defaults are
shown, and the shipped file has them commented out:

```
apps.backup.to:     /usb/backup
apps.backup.from:   /home /data /sys/zeitlos.cfg /sys/net.cfg
apps.backup.skip:   /data/ask *.tmp
apps.backup.keep:   10
apps.backup.verify: yes
apps.backup.eject:  no
apps.backup.name:   zeitlos
apps.backup.watch:  none
```

- `to`: the folder on the backup drive.
- `from`: folders and files backed up.
- `skip`: a path starting with `/` leaves that folder or file out;
  anything else is a name pattern (`*`, `?`); `none` for nothing. Always left out:
  `*.part` (files being saved), `/tmp`, `/ram`, `/usb`, `/nfs`, and the
  backup folder itself.
- `keep`: the newest backups that are never thinned ("Making room").
- `verify`: read every copy back before its backup is complete.
- `eject`: release the drive after every backup (as `backup -e`).
- `name`: one folder per computer, so several can share one stick.
- `watch`: folders or files (inside `from`) whose saves are backed up
  on their own while the window is open ("Watch"); `none` for none, as
  shipped. Paths with a space are quoted, as in `from`.
- Files that `update` installed and that are unchanged are not copied:
  `update` brings them back. Their CRC is in `/sys/manifest`
  ([update.md](update.md)); one you have edited is backed up like any
  other file.

## On the drive

```
/usb/backup/                               apps.backup.to
  README.TXT                               what this is, how to get files back by hand
  zeitlos/                                 apps.backup.name
    2026-10-19_18-00-12/INDEX.TXT          every file the card had; written last
    2026-10-19_18-00-12/home/book.txt      the first backup holds everything
    2026-10-19_18-00-12/home/notes.txt
    2026-10-20_09-15-40/INDEX.TXT
    2026-10-20_09-15-40/home/book.txt      later ones only what changed
    2026-10-21_11-30-48/INDEX.TXT
    2026-10-21_11-30-48/PINNED             pinned: never removed
    2026-10-21_11-30-48/home/book.txt
    2026-10-21_11-42-07w/INDEX.TXT         a watch backup: only the files it stored
    2026-10-21_11-42-07w/home/mybook/ch3.txt
    check/2026-10-21_11-42-07w-1.txt       copies `backup check -a` found damaged
    LOCK.TXT                               only while a backup runs
```

Everything is plain files under their own names, so any computer that
reads FAT can open them.

### Names

A backup is named by the local time it started, `YYYY-MM-DD_HH-MM-SS`
(dashes because FAT allows no `:`). Two in one second do not happen:
backup waits for the next second.

**When the clock is not set** the name is the newest backup's name with
`+NNNN` counting on: `2026-10-21_11-30-48+0001`, `+0002` ... No board
has a battery for its clock ([rtc.md](rtc.md)): without the network it
starts at 1970-01-01 00:00 at every power-on, so the clock's time would
put backups out of order -- a backup made 12 minutes after one power-on
would sort before one made 26 hours into the power-on before. The
counter cannot: each new name comes after every name already on the
drive. The same happens when the clock is set but behind the newest
backup. The window and `list` show such a backup as "after Wed 21 Oct
11:30 (clock not set)", and its INDEX.TXT says how long after power-on
it was made. A drive whose backups were all made without a clock starts
at `0000-00-00_00-00-00+0001`.

`w` at the end marks a watch backup.

Sorted by name, with the number after `+` compared as a number, the
folders are in the order the backups were made. (The four digits keep a
PC's plain sort right too, up to 9999.)

### INDEX.TXT

```
zeitlos-backup 1
backup 2026-10-21_11-30-48
kind full
time 2026-10-21 11:30:48
clock set
dates utc
from /home /data /sys/zeitlos.cfg /sys/net.cfg
skip /data/ask *.tmp
copied 3 812345
f 3a1b9c0e 1834 2026-10-21_09-29-50 2026-10-21_11-30-48 /home/book.txt
f 7c00d2a1 11 2026-10-19_17-58-02 2026-10-19_18-00-12 /home/notes.txt
s 1f2e3d4c 9000 2026-10-01_12-00-00 - /data/fonts/big.zfn
end 3 5e2f8a10
```

- `f crc32 size date where path`: a file, and the backup that holds its
  copy. `date` is the file's FAT date (UTC), `-` when it was written
  while the clock was not set (FAT then stamps 2020-01-01,
  `get_fattime()` in `sw/apps/fs/fs.c`).
- `s ...`: installed by `update` and unchanged; not copied.
- Sorted by path, bytewise. A reader skips keys it does not know.
- **`end <entries> <crc32>`**, the CRC-32 (zlib's) of every byte before
  it. A backup exists only when this line is there and right. A watch
  backup's index lists only the files it stored.
- `clock not set, 3h05m after power-on` instead of `clock set`.

## The rules that make a power cut harmless

The danger is real: FAT has no journal, and a drive that loses power
while it is being written can be left inconsistent. backup is built so
that this cannot cost you anything that was already safe.

1. **The card is only read.** A backup writes nothing on it -- FatFs
   writes nothing on a read -- so whenever the power goes, the card is
   as your programs left it. Only a restore writes the card, as
   `NAME.part` and a rename.
2. **On the backup drive nothing that exists is ever changed.** A
   backup only adds files: copies, and one INDEX.TXT. Nothing is renamed
   or moved there, ever. FatFs renames by writing the new directory
   entry before removing the old one; a power cut between the two leaves
   two entries on one cluster chain, and deleting either one later
   destroys the other's data. A backup cannot meet that.
3. **A backup exists only when its INDEX.TXT is complete.** One cut
   short has none, or one without its `end` line; it is invisible, and
   the next backup removes what it left. (A folder whose index lost its
   end but whose copies a later backup uses is not that -- its index was
   damaged -- and is kept and reported.)
4. **Every copy is synced and read back** before the index names it.
5. **Making room only deletes.** A cut there leaks space at worst.
6. **The window is short.** A typical backup writes for a few seconds;
   the drive is idle (and safe to unplug, after `eject`) otherwise.

What remains: a cheap stick's controller can damage data it was not
asked to write when the power fails mid-write (flash pages are larger
than sectors). Several copies on one stick do not help with that, or
with a dead stick -- they share one FAT and one directory tree. Two
drives used in turn do ("Later").

## How a backup runs

1. Take `LOCK.TXT` (one backup at a time; a lock whose owner is no
   longer running is taken over).
2. Remove what a backup cut short left.
3. Read the state the newest backups describe: the newest complete full
   backup's index, with the watch backups after it laid over it.
4. Look at the card. A file of the same size and date as before is
   unchanged. One of the same size whose date cannot be trusted (or has
   changed) is read and its CRC compared. Anything else is copied.
5. Check the room; make room if needed ("Making room"). If there is
   still not enough, **nothing is written**, and it says so.
6. Name the backup, make its folder, copy each changed file 4 KB at a
   time, sync it, read it back. A file that changed while it was being
   copied is copied once more, then reported.
7. Write INDEX.TXT, its `end` line last; sync; read it back.
8. Remove `LOCK.TXT`.

Nothing changed since the last backup: nothing is written.

Stopping (Esc, Cancel, `kill`) is immediate and changes nothing
visible: the backup has no index yet, and the next one removes its
folder.

### Fresh copies and repairs

Backups only copy what changed, so a file that never changes has one
copy, in an old backup. Two things keep that copy from being a weak
point:

- **A copy more than 10 full backups old is made again** (`refresh` in
  `bk_opts_t`), so no file depends on one old copy for ever.
- **`backup check -a`** reads every copy and checks it against its
  index. A damaged one is listed in `check/`, and the next backup copies
  that file again from the card, if it is unchanged there.

## Making room

When the drive is too full for the next backup:

1. **The oldest copies go first**, backup by backup from the oldest, but
   **a copy is removed only if a newer copy of that file exists** in a
   later backup. The newest copy of every file is never removed, so the
   oldest backup just shrinks to the files that never changed since.
2. Only if that is not enough: the last copies of files no longer on the
   card, oldest first.
3. Never: the newest `keep` backups (10) and everything after the oldest
   of them, and pinned backups. If nothing else can go, the backup is
   refused rather than eat into those.

A backup left with no copies at all is removed, index and folder. An
older backup thinned this way still lists everything in its index;
restoring from it says which versions were removed to make room.

## Getting files back on a PC

Without Zeitlos, by hand (README.TXT on the drive says this too):

- **One file:** look for it in the backup folders. Each folder that has
  it holds the version of that time.
- **Everything as of a time:** copy the backup folders up to that time
  over each other, oldest first. Files deleted later come back too.

Exactly, with Python 3 on any computer:

```
$ python3 tools/backup_read.py list /media/stick/backup
$ python3 tools/backup_read.py history /media/stick/backup /home/book.txt
$ python3 tools/backup_read.py restore /media/stick/backup ~/restored 2026-10-21
$ python3 tools/backup_read.py check /media/stick/backup
```

It reads only, checks every copy's CRC against its index, and follows
the same rules as `bk.c`: the test below checks the two against each
other.

## Drives

Any FAT12, FAT16 or FAT32 drive, as it comes; nothing is formatted or
changed on it but backup's own folder. Through FatFs, a USB stick takes
about 180 KB/s here ([usb_host.md](usb_host.md)): a first backup of
20 MB is about 2 minutes (4 with every copy read back); a typical one,
a few seconds.

**exFAT** (most sticks over 32 GB come so) cannot be read here: FatFs is
built without it (`FF_FS_EXFAT 0`). Turning it on was measured on
2026-10-21 with xPack GCC 15.2: `fs` grows from 33,497 to 40,170 bytes
stored (3.3K over its 36K budget) and from 76,192 to 86,800 in RAM
(10.6K over 75K), and `fs` is started at boot, where the RAM rule is at
1,024 of 1,024 KB. exFAT is also Microsoft's, which needs a legal look
before shipping. Left out for now. fs does not mount such a drive (its
console line: `fs: usb storage: mount failed`), and backup, which
cannot tell an unmounted drive from no drive without taking the drive's
events from fs, says both: "is the backup drive plugged in? (One that
is not FAT -- exFAT, NTFS -- is not mounted ...)".

## Watch

Set `apps.backup.watch` (Settings, **Watch**) to the folders or files
you work in -- `/home/book` -- and leave the window open, small or
shaded to its title. Each time a file there is saved, it is backed up
on its own, a **watch backup** (`...w` on the drive), once nothing more
has been saved for 10 seconds. Back up now still backs up everything;
watch backups come between, holding only what was saved.

```
Watch    /home/book: each save backed up 10 s after
Watch    ch3.txt saved: backed up in 7 s
Watch    backing up ch3.txt
Watch    /home/book: ch3.txt backed up at 14:40
Watch    /home/book: saves are not backed up while the drive is out
Watch    not watching: /etc is not in From, so not backed up
```

- **Only while the window is open** (or `backup watch` runs in a shell):
  no program runs in the background for it. Closing the window stops it.
- **Nothing is kept for later.** A save while the drive is out is not
  backed up by the watch (the line says so); the next Back up now has
  it. Nothing is remembered across the window closing.
- **The 10 seconds start again with every save**, so a run of saves is
  one backup. A file still open for writing is waited for -- its close
  is a change too -- for 60 seconds at most (a log kept open is then
  backed up as it is).
- **In Settings** a due watch backup waits until you are back.
- What can be watched: a path inside one of `from` (anything else
  would be thinned away as gone from the card), not on the backup
  drive, four at most for the whole system (fs keeps four watches for
  everyone). What cannot is said on the Watch line.
- In the backups: a watch backup's index lists only what it copied
  (`kind watch`); `backup files`, `history` and `restore` lay the watch
  backups over the full backup before them ("How a backup runs"), so
  `backup history PATH` shows every save. `backup status` shows the newest
  watch backup apart from the last full one.
- **`backup watch [PATH...]`** does the same in posix or at the kernel
  console, printing each watch backup as it happens; with no paths it
  watches `apps.backup.watch`. It runs until it is stopped (`kill
  backup0`) or its terminal closes.

**How it hears of a save.** fs tells only the programs that asked
(`Z_FSOP_WATCH`, [fs_driver.md](fs_driver.md#watching)) -- there is no
broadcast every program would receive -- and only about other
programs' changes: a file opened for writing there, closed after it,
deleted, or renamed into or out of it. It sends one notice and no more
until backup has read what changed (`Z_FSOP_WATCHREAD`), so a save is
never slowed and a busy folder cannot fill backup's mailbox; it keeps
four paths a watch between reads and says when there were more, and
backup then looks at the whole folder (copying only what changed in
it). backup also reads every 30 seconds, should a notice ever be lost.

## Running alongside everything

`fs` serves one request at a time, in the order they come, so backup
keeps one in flight and keeps each small: it reads and writes 4 KB at a
time, each its own request (zfsc.c's write-behind, which would send two
writes as one 8 KB request, is off: `-DZFSC_WBUF=0`) -- ~22 ms to a USB
stick at ~180 KB/s, ~7 ms from the card. A save in `text` waits behind
at most one of them. Target, to be measured on hardware: keystroke
latency and save time in `text` during a first backup as on an idle
system.

**One request can be long: the first free-space count.** backup asks
for the drive's free space once a backup (`STATVFS`). FatFs answers at
once from the drive's FSINFO sector when its count is valid, as a PC
keeps it. When it is not -- after a power cut it often is not ("On
FAT", below) -- the first ask after the drive is mounted counts the
whole FAT, during which `fs` answers nobody: a 32 GB stick's FAT is
about 4 MB, some 20 s at the stick's speed. FatFs remembers the count
after that. To be measured on hardware; if it matters, the count could
be done a step at a time.

## Inside

```
sw/apps/backup/bk.h, bk.c      the engine, portable C over bk_io_t
sw/apps/backup/bk_zfs.[ch]     its files: zfsc.c, straight to the fs driver
sw/apps/backup/bkwatch.[ch]    watch: fs's watches, the 10 s, what to back up
sw/apps/backup/backup.c        the app: settings, commands, the clock, the terminal
sw/apps/backup/bkjob.[ch]      the engine a slice at a time, in a coroutine (the window)
sw/apps/backup/bkwin.[ch]      the window
sw/apps/backup/tests/          memfs (a card and a drive in memory that lose power),
                               test_bk.c, test_fat.c, test_cli.c, test_job.c,
                               render.c (the window), test_read.py
tools/backup_read.py           the PC side
```

`bk_zfs.c` talks to the fs driver directly (`zfsc.c`), not through the
kernel's `Z_SYS_FS_*` calls, because those know only the card: the
backup drive's free space (`STATVFS /usb`) and `eject` (`UMOUNT`) are
the driver's, and so are listings a page at a time (`OPENDIR` /
`READDIR`) of any size. `backup.bin` is about 150 KB with its window
(xPack GCC 15.2; 61 KB of it zeroed memory), a card app in the `base`
set.

**Cheap to ask.** Listing the backups (`list`, the window's status, the
Restore list) reads only the last line of each INDEX.TXT (`end N crc`):
complete or not, and how many files. Reading every index whole, to
check its CRC, would be about a second a backup at a stick's speed. An
index is read whole, and its CRC checked, when it is used -- the state
a backup builds on, the files of a backup -- and by `check`. One that
turns out damaged then is said so and passed over: the full backup
before it is the base instead.

`bk_io_t` is the files (open, read, write, sync, stat, list, unlink,
mkdir, rename for restores, free space), the clock, the lock's owner,
output, progress and a stop flag. `bk.c` uses no `sscanf` and no 64-bit
`printf`, so it links with the integer-only printf apps use
([build.md](build.md)). It is about 33 KB of code and 5 KB of `.bss` on
RV32IM (`-Os`), and keeps the newest index and the card's file list in
memory: about 100 bytes a file.

## Testing

```
$ make -C sw/apps/backup/tests
test_bk: 849 checks, 0 failed
test_fat: 640 checks, 0 failed
test_cli: 39 checks, 0 failed
test_job: 3462 checks, 0 failed
test_watch: 150 checks, 0 failed
test_read: 17 checks, 0 failed
$ sudo sysctl -w vm.mmap_min_addr=0; make -C sw/apps/backup/tests render
render: 43 checks, 0 failed (pictures in render)
$ make -C sw/apps/fs/tests && sw/apps/fs/tests/test_fs | tail -1
test_fs: 10036 checks, 0 failed
```

`test_fat`, `test_cli` and `test_watch` need `mkfs.fat` and `fsck.fat`
(dosfstools).

`test_watch` runs `bkwatch.c` on the real fs driver's watches, with
another client pid saving: a save backed up 10 s after and alone, its
own writes (a restore) not heard, a run of saves as one backup, a file
open for writing waited for (and a log after 60 s), a rename-save, a
watched file and a file beside it, nine saves at once (more than fs
keeps: the folder, copying only what changed), more than 32 heard one
by one, a lost notice (read anyway at 30 s), the drive gone (dropped),
a saved-and-deleted file, restoring from a watch backup, and fs's four
watches given back and refused when taken. `test_fs` has fs's side
("Watching" in [fs_driver.md](fs_driver.md#watching)); `test_cli` runs
`backup watch` with a save in its third second.

With ASan and UBSan. `test_bk` covers names, a first backup, nothing
changed, a change, a deletion, history, restoring (beside, replacing, a
folder elsewhere, a damaged copy refused), pins, the drive unplugged,
backups without a clock and files with untrusted dates, the same
second, files `update` installed, watch backups laid over full ones,
fresh copies, `check -a` and repair, the lock, making room (oldest
first, the copy of a file that never changes kept, pins, `keep`, a
deleted file's last copy, refusing when nothing may go, the drive full
mid-copy), stopping, a damaged index (cut, and in the middle), and the
destination inside a source.

**Power cuts.** memfs counts every write to either volume and can lose
power at any one of them: that write happens or not, a file open for
writing keeps what was synced and some of the rest (perhaps with
rubbish at its end), and every call after it fails until a "reboot"
(clock not set, uptime 0). The tests run each of these once for every
write it makes, with the power going at that write:

| run | writes |
|---|---|
| a backup (changes, a deletion, a new file) | 26 |
| a watch backup | 16 |
| a first backup on an empty drive | 46 |
| a backup that has to make room first | 26 |
| a restore with `--replace` | 11 |

and after each: the card is byte for byte as it was; every backup that
was complete is complete and every copy it names is there and right;
`check -a` finds nothing wrong; and the next backup works and holds
exactly what the card has. After a restore cut short, the file is the
old one or the restored one (or only its `.part` is there).

### A slice at a time

`test_job` runs `bkjob.c` as the window runs it -- the engine in a
coroutine, resumed one slice at a time -- on the real fs driver, with a
folder whose listing takes several pages. It counts fs's requests in
every slice: a first backup of 161 files takes 4,014 slices, never more
than one request in any of them. Then it cancels a second backup at
each of 431 points in turn: every time it lets go within 20 slices (the
most at the very start, before the first look at the stop flag), the
lock is gone, the card is untouched, the next backup works and
`check -a` is clean.

`render` draws the window (`tests/render/*.pbm`: backed up, a backup
running, stopping, no drive, no backups yet, failed, no clock, after
Remove drive, Settings, the strip idle and running, and watch: watching,
saved, backing up, backed up, in the strip, with no drive) with the real
`bkwin.c`, its jobs and drive made up, and drives its keys, buttons and
the Aa icon: only the buttons that can be pressed are there, the focus
leaves with a button that goes and Back up now takes it back, Enter
starts a backup (and does nothing without a drive), Back up now during
a summary starts after it, Esc stops, a due watch backup starts with
just the saved file and a save with no drive is let go, F1 help.

### On FAT

`test_fat` runs the engine and the board's own file layer (`bk_zfs.c`)
through `zfsc.c` to the real fs driver (`sw/apps/fs/fs.c`) and the
project's FatFs, using `sw/apps/fs/tests/test_fs.c`'s kernel stubs: the
card a RAM disk fs formats, the backup drive a 40 MB FAT32 image made
by `mkfs.fat`, as a stick comes from a shop. It backs up, restores
(beside and replacing), fills the drive (refused before anything is
written; then, with the drive's FSINFO claiming room it has not got,
full in the middle of a copy: that backup fails and says so, the others
are untouched, and the next one works once there is room), and then
cuts the power at **every sector write** a backup makes, in turn: that
write lands in part (some whole sectors, perhaps half of one), nothing
after it does, and the drive is mounted afresh. After each, the same
checks as above, through the real filesystem.

A backup there makes 97 write requests. After 48 of the 97 cuts,
`fsck.fat` found the drive inconsistent -- and in no case was a backup
lost or a copy wrong:

| fsck.fat said | cuts |
|---|---|
| unused clusters reclaimed (space a cut left allocated to nothing) | 33 |
| the free cluster count (FSINFO) wrong | 16 |
| the two FATs differ ("Using first FAT") | 8 |

No cross-linked files and no damaged directories, which is what the
rules are for: a cut can leak space or leave a wrong count, and a PC's
`fsck` / `chkdsk` tidies those, but it cannot take what a backup
already has.

`test_cli` runs `backup.c` itself, command by command: settings from a
zeitlos.cfg (another folder, a quoted source, `skip none`), arguments as
the launch argument, output as the console gets it (`\r\n`), on the
same real filesystem -- no drive, `status`, `-n`, a backup, nothing
changed, `list`, `history`, `files`, `restore` (a quoted path, `-o`,
beside), `check -a`, pins, usage, `eject` and what comes after it.

**The tests fail when the code is wrong.** Each rule above was broken
in `bk.c` on purpose and the tests run: an index's CRC not checked, a
damaged index tidied away, making room removing newest copies, the
newest `keep` not protected, pins ignored, the destination or `.part`
files not skipped, the lock ignored, a restore not checking its copy --
each makes checks fail. (Two that did not were equivalent: without the
end-line test the CRC still rejects a cut index; memfs, like FatFs,
makes a file durable on close.) `test_read.py` was checked the same way
(watch backups not laid over, an index's CRC not checked, a name prefix
picking the oldest match), and `bk_zfs.c` too (no free space but the
total, a full drive not reported as full, an empty listing). Two of its
mutants changed nothing and were left: EEXIST is mapped twice, and
FatFs's close flushes as its sync does.

## On the board

Not run on hardware yet. To check there, collected for the final
round:

- The window (the dock, `backup window`, `run backup`): each state in
  the table ("The window") -- plug the drive in and pull it out: the
  headline follows within a second or two, and the buttons with it;
  Back up now, Esc in the middle, typing and moving windows during a
  backup (nothing waits); Show backups opens files at the drive, with
  README.TXT; Remove drive; Settings (a value changed and in
  zeitlos.cfg, every other line as it was); Aa and back; shaded titles;
  F1.
- The coroutine's stack on RV32: `free` during a backup from the window
  (32 KB in bkwin_run()'s frame; the host used 16.5 KB).
- Watch (`apps.backup.watch: /home/book`, the window small in a
  corner): save in `text` -- 10 s later the Watch line says it was
  backed up, the titlebar too when shaded, and History in Restore shows
  the save. Typing and saving feel no different with the window open.
  Several saves in a row: one watch backup. The drive pulled: the line
  says saves are not backed up; plugged in again, the next save is.
  A path outside From and a fifth watch: said, not watched. `backup
  watch /home/book` in posix, a save from the desktop: its lines
  appear; `kill backup0` ends it.
- The dock icon: there with a base card, and starts the window.

- `backup` from posix and `run backup` at the console; `status`,
  `list`, `history`, `restore` beside and `--replace`, `check -a`,
  `pin`, `eject` (then the stick can be pulled; `usbmount` or replugging
  brings it back).
- A first backup of a full base card to a FAT32 stick: time, KB/s, and
  `free` during it (the 512 KB it asks for).
- The first free-space count on a stick whose FSINFO is not valid
  (a power cut, or `fsck` on a PC that left it unset): how long `fs` is
  busy.
- Typing and saving in `text` during a first backup: keystroke latency
  and save time against an idle system.
- Pull the stick in the middle of a backup; switch the power off in the
  middle of one: the next backup says it removed the one cut short, and
  `check -a` is clean. The card's files unchanged (`backup check`,
  `fsck` of the card on a PC).
- `kill backup0` in the middle: as above.
- A backup without the network (clock not set): `+0001` names, shown as
  "after ..."; with the clock set again, plain names.
- The drive read on a PC: folders open, README.TXT, and
  `tools/backup_read.py restore` gives the card's files.

## Decisions

Made with the user, 2026-10-21 (design notes: `claude/backup-proposal.md`
in the project).

- **Snapshots, not a mirror**, after archiv (github.com/machdyne/archiv),
  simpler.
- **One folder per backup holding what changed** (archiv's layout),
  rather than one folder per file with its versions inside: browsable by
  date on a PC. Made safe on FAT by never moving anything: making room
  removes a copy only when a newer one exists.
- **Rejected: full copies every time.** Copies on one stick share one
  FAT and directory tree, so the common failures (a torn FAT sector, a
  damaged directory, a dead stick) hit all of them; full copies cost
  minutes per backup (a long power-cut window), ten times the space, and
  wear.
- **Rejected: two FATs** when Zeitlos formats a drive: backup takes any
  FAT drive straight out of the box.
- **By hand, by default**: a button or `backup` (philosophy.md, Tool:
  the system acts only when asked). **Rejected:** backing up everything
  on every save; backing up when a stick is plugged in; a separate watch
  process (the window watches while it is open); remembering saves while
  the drive is missing.
- **fs notifications only to programs that ask**, not a broadcast every
  program would receive; one notice until it is read, so fs never waits
  and a mailbox never fills.
- **The window is one small status window** (2026-10-21, redone):
  a headline, Back up now, only the buttons that can be pressed; no
  restore browser -- files are copied back from the drive, or `backup
  restore` -- and the Aa icon for a strip instead of Small and Full.
  The drive folder, Skip, Keep and eject stay in `zeitlos.cfg`.
- **Not now: two drives in turn.**
- **Watch lives in the window** (and `backup watch`), 10 s after the
  last save; no pending saves when the drive is out.
- **Names are the date**, with `+N` when the clock is not set. Rejected:
  1970 dates (out of order across power-ons); run numbers in every name.
- **exFAT: not now** (measured above).
- **A card app** in the `base` set, not in flash; settings
  `apps.backup.*`.
