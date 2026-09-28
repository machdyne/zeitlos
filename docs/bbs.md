# bbs: a bulletin board system, and fed: federation

**Status: being built, in phases. Phases 1 (netserve), 2 (term), 3
(the BBS core) and 4 (messaging) are done: the BBS runs, on Zeitlos and
on Linux, with accounts, bulletins, forums, private mail, a new-scan,
who's online, profiles and sysop tools. Federation is next.** This page is the plan, the decisions behind
it, and -- from "Running it" down -- the BBS as it is. Each phase
updates it as it lands. The Linux server: [bbs_linux.md](bbs_linux.md).

The motivating deployment is **Machdyne BBS**, `bbs.machdyne.com`,
which currently runs Mystic and will move to this software -- hosted
on a Linux server for availability and more simultaneous callers --
with the sysop's own Zeitlos machine running a second node, not
necessarily reachable from the internet, that is fast to use locally
and whose posts go out to the network.

## What it is

- **`bbs`** (`sw/apps/bbs`): the BBS. A port provider, `bbs0`, reached
  over telnet and SSH through [netserve](netserve.md) (`noauth`
  listeners), or locally with `port bbs0` in `term`. Users and
  passwords, system bulletins at login, private messages, message
  forums, door games. No file areas (below: "Files").
- **`fed`** (`sw/apps/fed`): federation. Signed objects on topics,
  exchanged between nodes that each choose their own peers -- a
  publish/subscribe network with no master instance, of which forum
  posts are one kind of object.
- **Portable.** Both have a core that includes no Zeitlos headers, and
  a thin platform layer: one for Zeitlos, one for Linux. The same data
  directory works on either.

## Decisions so far

| | decided | why |
|---|---|---|
| Reaching it | netserve `noauth` listeners; the BBS logs callers in itself | telnet needs an in-band login anyway, so SSH does the same: one login path. No delegation protocol between netserve and the BBS |
| SSH without credentials | accept `none`, any password, any key ([netserve.md](netserve.md#noauth)) | every client connects, whatever it tries first; the session is still encrypted |
| Safety of `noauth` | sessions are marked unauthenticated; shells refuse them ([ports.md](ports.md#who-is-connecting)) | a misconfigured listener fails closed, in the program that would hand out a shell |
| Config lists | `;`-separated | commas are legal in FAT names |
| Passwords | salted, iterated SHA-256 | simple and enough for a BBS; `zsha256` is already here. No Argon2 |
| Screens | one design for every terminal: layout, menus and meaning carried by text, position and reverse video; colour decoration only. Box drawing in UTF-8, which `term` draws ([terminal.md](terminal.md#line-drawing)); CP437 for callers who ask for it | `term` is 1bpp; the same screens must work there and in SyncTERM |
| Charsets | UTF-8 inside; per caller UTF-8, CP437 or ASCII at the edge | SyncTERM and friends expect CP437 |
| ANSI art | optional sysop files, not designed around | colour art does not survive 1bpp; shade blocks (dithered) do |
| Federation | a native protocol first; an FTN gateway optional, later | no master, peers chosen locally, reliable after downtime. FidoNet's addressing is assigned hierarchically. ActivityPub needs an always-on HTTPS server and is push-based |
| Federated content | typed, signed objects on topics (pub-sub): messages and events, not files | door leagues, bulletins, node lists, announcements (a Zeitlos update, told to the OS itself) ride the same network |
| Files | **not carried** -- neither file areas in the BBS nor files over fed | too big for LoRa, and the web, FTP and the like move files better. Perhaps later as an optional feature for other networks: off by default, sizes limited -- and not on ours |
| Doors | last: after the live server and the rest of federation; correspondence chess as a demo | |
| Names | the apps: `bbs`, `fed`. The network defined by Machdyne's node list: open ("Timeless Network" is a candidate). The protocol: to be named before phase 5 fixes it in the handshake | |

## Phases

| # | phase | status |
|---|---|---|
| 1 | **netserve**: listener lists; per-listener `noauth`, `any`, `subnet`; SSH `noauth`; the identity map; shells refuse unauthenticated sessions; `apps.netserve.ssh_sessions` | **done** -- [netserve.md](netserve.md) |
| 2 | **term**: private modes (`CSI ? 25 l` used to print), save/restore cursor, insert/delete/erase character, the cursor-position report and device attributes answered, DEC special graphics, RI/IND/NEL, SU/SD, full reset; box drawing, blocks and shades as cell codes, drawn at the cell size with dithered shades | **done** -- [terminal.md](terminal.md#escape-sequences-the-emulator-implements) |
| 3 | **BBS core**: portable core and platform layers (Zeitlos, Linux); `bbs0` with one state machine per caller; the output layer and terminal detection; users, signup, login; bulletins; menus; profiles; who's online; the local sysop; config; Linux deployment (systemd, OpenSSH `ForceCommand`) | **done** -- below, and [bbs_linux.md](bbs_linux.md) |
| 4 | **Messaging**: message base (append-only log and index, one writer), private mail, forums, new-scan, last-read, reader, editor with quoting | **done** -- "Messages", below |
| 5 | **fed core**: signed objects on topics, node lists, the hybrid (post-quantum) session, the store, catch-up after downtime, outbound-only nodes, the local interface -- [fed.md](fed.md) | **done** on Linux and Zeitlos; first light on hardware |
| 6 | **Federated BBS**: forums over fed; mail between nodes, sealed; cancels and moderation; retention | **forums, mail, cancels and moderation: done** ("Federation", below); retention next. (No posting as someone on another node: an address is `handle@node`, and each node vouches only for its own users -- fed.md, "One person, several nodes") |
| 7 | **Network profiles** -- LoRa's groundwork: a network's maximum object size and its cryptography, in its node list; a link's size limit, per peer; letters sealed classically where a network chooses it (fed.md, "Constrained links") | **done** |
| 8 | **The live server**: bbs.machdyne.com configured and running -- the BBS and `fed` on Linux, the end-to-end zfed test with a Zeitlos board | |
| 9 | **More over fed**: network bulletins; **events** -- announcements to the OS itself (a Zeitlos update, say), signed by a publisher the network names; **LoRa** through `mesh`; an optional FTN gateway | |
| 10 | **Doors**: the door protocol, a `zdoor` helper, dropfile, relay and time limits; blackjack, then a multiplayer poker table; door leagues; correspondence chess, as a demo | |

**No migration** from the Mystic BBS: its users will be asked, on the
old board, to make new accounts; its messages kept as a plain text
archive, for history, not imported -- they would not be linked to users.

**Files** are not planned: not file areas, not files over fed (the
decision above). If other networks want them, they could come later as
an optional feature, off by default and size-limited, and not enabled
on ours.

## What the platform imposes

Found while planning, and worth knowing before any code:

- **Callers at once:** eight TCP connections in the system, two kept
  for outbound; netserve holds six sessions, SSH ones from its pool of
  `apps.netserve.ssh_sessions`. Four to six callers on a large board,
  two or three on a 1MB one -- the Linux host is for more.
- **One process, a state machine per caller.** No threads; anything
  that blocks (a hash, a file read) pauses every caller briefly.
- **Files:** FatFs with eight handles for the whole system, no locking,
  and no rename of a file open for writing
  ([filesystem.md](filesystem.md)). Open briefly; append-only logs whose
  index is written last; write-then-rename; one writer per file.
- **The screen is 80x25**: netserve refuses NAWS, and `term` is that
  size.
- **Throughput** is about 11 KB/s ([networking.md](networking.md#known-limits)).

## Running it

### On Zeitlos

`bbs` is an app (`sw/apps/bbs`) that registers the port `bbs0`. Its
files live in one directory, `/bbs` unless `apps.bbs.dir` in
`/zeitlos.cfg` says otherwise ([config.md](config.md)); it makes the
directory on first start. For callers from the network, a `noauth`
listener in netserve ([netserve.md](netserve.md#listeners)):

```
apps.netserve.telnet: 23 repl0; 2323 bbs0 noauth any
apps.netserve.ssh: 22 posix0; 2222 bbs0 noauth any
```

netserve starts `bbs` when the first caller arrives, so nothing else is
needed for them. **Locally**, start it yourself -- `run bbs` in posix,
`(run "bbs")` in the REPL -- then `port bbs0` in `term`'s Open bar
(`term`'s Open bar connects; it does not start programs,
[terminal.md](terminal.md)). A local call is like any other: it logs in.

The **first account made is the sysop's** (level 255, the `S` item on
the menu): make yours before you open the listener to the world.

`bbs` is in the LARGE memory tier (see "Memory" below).

### On Linux

The same core as a server: telnet itself, SSH through OpenSSH, local
calls through `bbs --connect --local`. [bbs_linux.md](bbs_linux.md).

### The data directory

Everything the BBS keeps, in one place, the same on either platform --
copy it between a card and a server and it works:

| | |
|---|---|
| `bbs.cfg` | settings (below). Optional |
| `users.dat` | the accounts, 256 bytes each (below) |
| `bulletins/` | `*.txt` and `*.ans`, shown in name order |
| `text/` | screens that replace the built-in ones: `welcome.txt`, `menu.txt` |
| `forums.cfg` | the forums (below); a first one is made if missing |
| `node.id` | this BBS's id, 16 hex digits, made once (below) -- keep it with the messages |
| `msgs/` | each area's `.log` (the messages) and `.idx` (its index) |
| `lastread/` | `<user id>.txt`: where each user is in each forum |

`sw/apps/bbs/data` is a starting point: a commented `bbs.cfg`, a first
bulletin, and a note on `text/`.

## Configuration

`<datadir>/bbs.cfg`, `key: value` lines like `/zeitlos.cfg`. Not keys in
`/zeitlos.cfg` itself, because the same directory has to work on a
Linux server, which has none. A `#` with blanks on both sides starts a
comment, so `name: Board #1` keeps its `#1`. Unknown keys are logged and
ignored; numbers are clamped to their range. Read at start.

| key | default | |
|---|---|---|
| `name` | `Zeitlos BBS` | on every title bar and the welcome screen |
| `sysop` | (none) | shown on the welcome screen; informational -- the sysop is whoever has level 255 |
| `nodes` | 4 | callers at once, 1-16. On Zeitlos each is ~5KB, and netserve's six sessions are a limit anyway |
| `new_users` | `yes` | may callers make accounts (`NEW` at the prompt) |
| `idle_minutes` | 15 | logged in and silent this long: goodbye |
| `login_seconds` | 120 | to get logged in (with some grace while typing) |
| `pw_iterations` | 2000 | SHA-256 rounds for passwords set from now on (below) |
| `new_level` | 10 | a new account's level |

## Callers' terminals

Every call starts with one question to the terminal: the BBS prints
`─` (three bytes of UTF-8) and asks where the cursor went (`CSI 6n`).

| answer | means | from then on |
|---|---|---|
| column 2 | the three bytes were one character: UTF-8 | UTF-8, ANSI |
| column 4 | three characters: an 8-bit terminal | CP437, ANSI -- SyncTERM, NetRunner, the DOS tradition |
| none in 2.5 s | no ANSI | plain ASCII: line drawing as `- \| +`, no escape sequences at all |

With an answer, a second question finds the screen's size (the cursor
sent to 999;999). Zeitlos's own `term` answers both since phase 2. A
terminal that answers nothing shows `─` and a few characters of the
question as a little noise before the welcome screen; that is the price
of asking, and why it is asked once.

Each user can override what was found, in **Profile and terminal**:
characters (detect, UTF-8, CP437, ASCII), colour (detect, on, off) and
rows. The profile screen has a box test -- two boxes and four shades --
to see which is right.

Input is translated too: a CP437 caller's `ü` (0x81) arrives as ü, so a
handle typed on SyncTERM is found when typed on `term`.

## Screens

**One design for every terminal** (the decision above). A screen's
meaning is in its text, its layout and reverse video; colour, where the
terminal has it, only tints. Menu keys are in brackets, `[B] Bulletins`,
so they read the same in colour, in black and white (Zeitlos's `term`
is 1bpp) and in plain ASCII. Title bars are reverse video,
` name │ title`. Characters are the ones every terminal here can draw:
Latin-9 and line drawing -- **not** an em dash or curly quotes, which
Latin-9 lacks and `term` would show as its missing-glyph box (the tests
check for that).

**Pipe codes** in bulletins and `text/` screens -- the codes Mystic,
Renegade and friends use, so imported text keeps its look:

| code | |
|---|---|
| `\|00`-`\|15` | foreground colour, DOS order (`\|07` grey, `\|15` white, `\|12` bright red) |
| `\|16`-`\|23` | background colour |
| `\|CL` `\|CR` | clear the screen; a new line |
| `\|BN` `\|SN` | the BBS's name; the sysop |
| `\|UH` `\|ND` | the caller's handle; their node |
| `\|DA` | the date and time now, UTC |
| `\|RV` `\|RO` | reverse video on, off -- ours: meaning that survives a terminal without colour |
| `\|\|` | a `\|` |

Anything else after `|` is shown as it is. Colour codes do nothing for
a caller without colour.

**Text from callers is never interpreted.** A handle, a location --
anything a caller typed -- is written without pipe codes and without
control characters, so nobody can clear everyone's screen from their
profile. The line editor refuses control characters as they are
typed, and handles refuse `|` and `@` outright (the latter joins a
handle to its BBS, phase 6).

**Bulletins** are `bulletins/*.txt` (UTF-8, pipe codes) and `*.ans`
(CP437 ANSI art: escape sequences passed to ANSI callers, dropped for
others; a SAUCE record is not shown). They are shown in name order,
titled by their name without a leading number: `01-house_rules.txt` is
"house rules". At login a caller sees the ones **changed since their
last call** -- all of them the first time -- and `B` shows them all.
Long ones are paged: Enter goes on, Q stops.

**Replacing screens:** `text/welcome.txt` replaces the welcome screen,
`text/menu.txt` the main menu (list the keys yourself: N F M B W U P G,
and S for sysops). Keep them to one screen.

## Messages

### Forums

`<datadir>/forums.cfg`, one forum a line, fields separated by `;`:

```
# tag; name; level to read; level to write; description
general; General; 0; 10; Anything at all
announce; News; 0; 255; From the sysop
```

The **tag** is the forum's identity -- its files are `msgs/<tag>.*`, and
it will be its name on the network (phase 6) -- so it is lower-case
`a-z 0-9 - _`, at most 16, and never reused for something else.
The name and description are for people and can change. `mail` is taken.
Up to 31 forums; the order of the file is the order on screen. Read at
start. A missing file is made with `general` and `zeitlos`.

A caller sees the forums their level may read, numbered as they see
them; `W` appears only where they may write.

### Reading

- **`N` on the main menu: the new-scan.** Unread mail first, then every
  forum with something new, in order, message by message. `Q` stops.
- **`F`: the forums**, with how many are new and in all; a forum's menu
  reads the new ones, or all from the start.
- **`M`: your mail.** Only letters addressed to you. The main menu says
  when some are new.

A message shows its forum, number, writer, recipient (mail), subject,
date (UTC), and what it replies to, then its body a page at a time.
Then: **N**ext (or Enter), **P**revious, **R**eply, **D**elete, **Q**uit.

**Message text is a caller's text** and is shown as it is: pipe codes
are not interpreted, control characters are not sent. Colours in
imported messages would need a decision of their own (there is no import: see "No migration").

**Last read** is kept per user per forum, in `lastread/<id>.txt`
(written to a new file and renamed over the old, when leaving a forum
and when logging off). A forum's "new" count is the messages after it,
deleted ones included. Mail is "read" by a flag on the letter itself.

**Deleting** is marking: the message stays in the log, flagged in the
index, and is skipped from then on. Its writer may delete it, the
recipient of a letter may, and the sysop may delete anything.

### Writing

`W` in a forum's menu, or in the mail's (which asks for a handle
first), or `R` on a message. A reply takes `Re:` and the subject, is
addressed back to the writer of a letter, and may quote the original.

**Full-screen, with ANSI**: [zetta](zetta.md), nano-style -- To (for a
letter, checked against the users when it is left, and again on
sending) and Subject at the top, the text wrapped as it is typed, the
keys along the bottom:

| | |
|---|---|
| `^S` (or `^Z`) | posts -- Send, for a letter |
| `^Q` | (a reply) the quote window: the original's lines, Space to choose, Enter to put them in after `> ` (`>` before a line already quoted) |
| `^X` | leaves -- asking "Discard this message?" when something is written |
| `Esc` | a menu of everything |

It is stored as lines of at most 79 columns, the same way on as the
line editor's `/s`: local forums, mail and federated forums alike.
While writing, a caller's text (6 KB on Zeitlos) and ~5 KB for the
editor, its cut buffer and the original replied to are allocated, and
freed when the writing ends or the caller hangs up (`tests/test_bbs.c`
measures the heap across a hang-up mid-message).

**A line at a time**: without ANSI, and for anyone who chooses it in
their profile (`P`, then `E`: "Editor: full-screen, or a line at a
time"). It works on every terminal:

| | |
|---|---|
| a line and Enter | adds it; a word that would pass 76 characters moves to the next line by itself |
| `/s` | saves |
| `/a` | abandons |
| `/l` | lists what is written, numbered |
| `/d 3` | deletes line 3 |
| `/?` | the help |

A message is at most 200 lines and 8 KB (6 KB on Zeitlos); the editor
says when it is full. Its buffer exists only while someone writes, and
is freed however the writing ends -- saved, abandoned, or the caller
hanging up.

### Storage

Each area is two files, and the BBS is their only writer:

- `msgs/<tag>.log`: the messages, appended, never rewritten. Each is
  `ZM1 <length>`, `key: value` headers, a blank line, and the body --
  plain UTF-8, readable with any viewer:

  ```
  ZM1 184
  id: aa55d96374bb5231:general:2
  from: Anna
  from_id: 2
  subject: Re: First post
  date: 1790505396
  reply: 1
  reply_id: aa55d96374bb5231:general:1

  Phil wrote:
  > Hello from telnet.

  Hi Phil!
  ```

  Headers this version does not know are kept and ignored, so later
  versions can add some. A header value is one line: a newline in one
  becomes a space, so a subject cannot forge a `from:`.
- `msgs/<tag>.idx`: 32 bytes a message -- where it is in the log, its
  length, date, writer, recipient, what it replies to, and its flags
  (deleted, read) -- so lists and scans need not read the log.

**The log is written before the index.** A crash between the two
leaves the log ahead; at start, each area's index is compared with its
log and the missing messages indexed from it. Only whole records count
-- one that parses, fits, ends with its newline, and is followed by the
end of the log or another record -- so a torn write is not taken for a
message; the next message is written over it. Index entries that point
past the end of the log (a log restored from an older copy) are
ignored, and logged.

**Message ids** are `<node id>:<area>:<number>`: where the message was
written, and its number there. Unique without anyone handing numbers
out. `node.id` is 16 random hex digits made when the BBS first starts;
**keep it with the messages** -- copied to a new machine, the board is
the same board. These ids are provisional until the `fed` spec (phase
5), which will tie a node id to the node's key. A reply names what it
answers both by number (`reply`, meaningful here) and by id
(`reply_id`, meaningful anywhere).

## Federation

A forum can be carried over zfed ([fed.md](fed.md)): posts written on
any node of the network appear on all of them. On Linux, and on Zeitlos
(host-tested; on a board with the rest of zfed, fed.md's step 6).

**Configuring.** `bbs.cfg` says where this node's `fed` is, and a
forum's sixth field in `forums.cfg` is its topic:

```
# bbs.cfg
fed: /var/lib/fed/fed.sock          # on Zeitlos: fed0

# forums.cfg
general; General; 0; 10; Anything at all; timeless/forum/general
local;   Local;   0; 10; This node only
```

A forum without a topic stays on this node. Every federated forum must
be in one network (the topic's first segment); a line naming another is
logged and its forum stays local.

**How it works** (`core/fedlink.c`):

- The BBS connects to `fed`, asks for this node's key (`KEY`), and
  subscribes to `<network>/*`. It reconnects every 5 seconds if `fed`
  is not there. On Zeitlos `bbs.c` is a client of `fed0` as well as the
  provider of `bbs0`: it connects in its loop (a blocking connect would
  drop callers' messages while it waited), notices within a second if
  `fed` dies, and tells `fed`'s messages from callers' by **sender** and
  connection id -- the ids alone can be the same number.
- **A post to a federated forum is published** (`PUB`), as JSON --
  `from`, `subject`, `date`, `body`, and `reply` (the parent's object
  id) and a random `post` token -- and enters the forum **when `fed`
  delivers it back**, under its object id: one way in for every post,
  local or not. The caller is told "Sent to the network", or, with
  `fed` not reachable, "Waiting for this node's fed".
- **Until `fed` says OK, a post waits in `<datadir>/fed-outbox`**, and
  is sent again after any reconnect. Sent twice it would be two objects
  (`fed` stamps each with its time), so the token makes the second one
  a repeat.
- **Delivery is at least once**: an object whose id -- or token -- is
  among the forum's last 500 messages is acknowledged and skipped.
- **Authors** show as `handle@node`, the node's name from the
  network's node list (kept in `<datadir>/fed-names`), or its short id.
  This node's own posts show as the plain handle.
- **What arrives is from other people's machines**, for callers'
  terminals: control characters are taken out -- the C1 ones too, since
  U+009B is a whole CSI to some terminals -- and an `@` in a remote
  handle becomes `_`, so nobody can pose as someone on another node. A
  date more than a day from the object's own time is replaced by it.
- A reply names its parent by object id; a parent that is not in the
  forum here (older than its retention, say) makes it a new thread.

**Mail between nodes.** A letter's To is a handle here, or
`handle@node` -- a node in the network's list (checked as the field is
left: "No node by that name on the network."); `handle@<this node>` is
simply a letter here. The letter goes to `fed` as `MAIL`, through the
outbox like a post, and `fed` seals it to that node (fed.md, "Mail
between nodes"): every node may carry it, only that one can read it.
On the way in, a letter on this node's mail topic goes to `fed` to
OPEN, and is acknowledged only once it is stored -- in its addressee's
mailbox, from `phil@alpha`, under its object id (the same one twice,
by id or by token, is one letter). Replying to it writes back the same
way.

- **No such user here**: it goes back to the sender's node, a letter
  from `postmaster`, "Not delivered: ...", saying why -- never in
  answer to a postmaster's own letter, or two nodes could answer each
  other for ever.
- **fed will not send it** (the node's info -- its mail keys -- not
  arrived yet, say): a notice in the writer's own mailbox, "Not sent:
  ...", saying to whom and why. Nothing is lost without a word. The same
  for a post fed refuses -- larger than its network takes (fed.md,
  "Network profiles"): "Not posted: ...", naming the forum and why.
- A letter fed has to OPEN while the link's output is full waits in the
  input, unacknowledged, and is tried again as the output drains.

**Deleting a federated message** (`D`: its writer, or the sysop) also
sends a **cancel** (fed.md, "Moderation"). Other nodes honour it where
this node is the message's origin -- its writer is here -- or a
moderator of the forum: then it is deleted there too. A sysop deleting
someone else's message deletes it here; the cancel goes out, and is
honoured nowhere else. A cancel arriving for a message here is put to
`fed` (`CANCELLED`), and the message deleted if it honoured it; the
cancel is acknowledged after. `tests/live_fed.py`: beta's sysop deleting
phil's post on beta -- and it staying on alpha; phil deleting his own --
and it going on beta too; beta's own post untouched.

`tests/live_fed.py` sends real letters over telnet, in zetta: phil on
alpha to `anna@beta` (arriving from `phil@alpha`, to anna, whole), and
anna's answer back; **its text in neither node's fed store** -- which
does hold the sealed letter -- only in the addressee's mailbox; a
letter to `nobody@beta` back from `postmaster@beta`; `anna@nowhere`
refused at once. `fed` publishing letters unsealed fails it.
`tests/test_zport_fed.c`: fed refusing a letter -- the notice in the
writer's mailbox, to whom and why, and the letter out of the outbox.
That test found a real bug: the notice read the refused letter from the
outbox as a string, which it is not (a length, no NUL) -- the JSON ran
into stale bytes, and the notice was dropped without a word.

**Memory**: the link adds ~87 KB of static data on the board: an object
in (18 KB) and out (17 KB), one work buffer (17 KB) for everything that
is never needed at once -- an outbox record, a post being built, a post
arriving, the names file at start -- the parser's 1,024 tokens (24 KB)
and the names (10 KB). (Its first version held ~200 KB, separate copies
of that one buffer among it: measured with `nm` on `bbs.elf`, and cut.)
Names are kept for lists of up to ~110 fully described nodes, keyed by
the first 16 hex digits of a node's key -- a clash could only show a
wrong name; who is accepted is `fed`'s business, in full.

**Testing**: `make -C sw/apps/bbs live` (`tests/live_fed.py`, 18
checks): two nodes, each a `fed` and `bbs-linux`, callers on telnet. A
post on one reaches the other's forum as `handle@node` with its whole
body; it is on its own node as the plain handle under its object id;
one back the other way; a local forum's post stays local; with `fed`
stopped a post waits in the outbox and goes when it is back; a hostile
post -- an escape, a C1 CSI and a bell in it, `anna@beta` as its author
-- arrives clean and as `anna_beta@alpha`; one post published twice is
one message. The C1 filter, the `@` rule, the token and the outbox's
clearing were each broken on purpose, and each fails it.

`tests/test_zport_fed.c` (in `make test`) runs the real `bbs.c` against
a scripted kernel and `fed`: the CONNECT to `fed0`, KEY and SUB; an
object delivered in two pieces, stored as `carol@<short id>` and
acknowledged; a post PUBlished; a caller whose connection id is `fed`'s
(1) whose data still reaches the caller; `fed` closing -- back five
seconds later, the unanswered post sent again; `fed` dying -- noticed,
and back when it is. Telling messages apart by id alone, retrying at
once, and not noticing a dead `fed` each fail it.

## Users

`users.dat`: fixed 256-byte records, user *n* at byte (*n*-1) x 256,
fields written one at a time little-endian so the file reads the same
on RV32 and on x86. A change is one seek and one write of one record; a
new user is appended. Nothing is ever deleted -- a disabled account
keeps its record, id and handle, so messages that name it keep meaning
it.

**Handles**: 2 to 20 characters, any language; letters, digits, spaces
(one at a time, none at the ends), `- _ .`; no `@` or `|`; `new`, `all`
and `everyone` are reserved. Unique without regard to case (ASCII
letters folded).

**Levels**: 0-255; 255 is the sysop. New accounts get `new_level`. Only
the sysop level means anything yet; messaging (phase 4) will use the
rest.

**The sysop** (`S` on the menu): find a user by handle, set their
level, set a new password, disable or enable them (not yourself; nor
can you lower your own level -- another sysop can), and disconnect a
node.

**Logins**: three wrong passwords, five unknown handles, or Enter alone
at the handle prompt three times, end the call; each wrong password is
logged with the caller's address.

## Passwords

Salted, iterated SHA-256: *h* = SHA-256(salt ‖ password), then *h* =
SHA-256(*h* ‖ salt ‖ password), `pw_iterations` times in all. The salt
is 16 bytes from the TRNG (`bbs` warns at start if the TRNG is not
seeded, [trng.md](trng.md)); the iteration count is stored per user, so
raising it applies to passwords set afterwards and old ones still work.
Compared in constant time.

A hash **blocks every caller** while it runs -- one process, no
threads. **Measured on the board** ([cryptobench.md](cryptobench.md#results)):
2000 rounds, the default, takes **4.26 s** -- the slow login, and four
seconds in which nobody else on the BBS gets a character; 200 rounds
takes 0.42 s. On a Linux server 2000 rounds is a millisecond or two.
With the SHA-256 block ([sha256_hw.md](sha256_hw.md)) the same hash
goes through hardware, `bbs` unchanged: **2000 rounds in 0.63 s**
(measured), so the default is fine on a board with the block. On a board without the block, and until the
hashing is made incremental, `pw_iterations: 200` in `bbs.cfg` is the
setting; passwords already set keep their own count until they are
changed.

## Memory

On Zeitlos, `bbs` is in the **LARGE** tier (64KB of stack and heap,
`sw/os/kernel.h`). Each node is about 6KB, most of it a 4KB output ring;
four nodes by default. Someone writing a message has a 6KB editor
buffer besides, allocated when they start and freed when they stop; if
there is no memory for one, they are told to try again in a moment. Each caller can have eight 512-byte sends
waiting for acknowledgement. A page of text stops early, with its "go
on" prompt, rather than let the ring overflow, so a dense page is never
cut off. On Linux the ring is 16KB.

## How it is built

```
sw/apps/bbs/
  core/        the BBS: portable C99, no Zeitlos headers
    bbs.h        the core's API and the platform's (plat_*)
    session.c    the callers: detection, login, joining, menus, pager, sysop
    reader.c     forums, mail, the new-scan, the reader, the editor
    msgbase.c    the message base: areas, log and index, repair, last read
    out.c        output: charsets, pipe codes, title bars
    users.c      users.dat, passwords, handles
    cfg.c        bbs.cfg
    text.c       UTF-8, CP437, dates
  bbs.c        Zeitlos: the port provider bbs0
  linux/       Linux: the server (telnet, the socket, --connect)
  (the platform -- files, clock, TRNG, log -- is sw/common/zplat.h,
   zplat_zeitlos.c and zplat_posix.c, shared with zfed's fed)
  data/        a starting data directory
  tests/       host tests
```

The core never waits. Input arrives with `bbs_input()`, output leaves
through `bbs_output()`/`bbs_consumed()` at whatever pace the connection
takes it, and `bbs_poll()` runs the timers. On Zeitlos, `bbs.c` follows
[ports.md](ports.md)'s rules: messages matched by sender and tag,
strangers rejected, acks counted after close, dead peers found by a
check once a second.

## Testing

```
make -C sw/apps/bbs test
```

- `tests/test_bbs.c` (146 checks): the core on the Linux platform with a
  fake clock. Its UTF-8 caller is `sw/common/zvt100.c`, the emulator
  `term` uses: it answers the detection probe by itself, and checks are
  made on the **screen as it would look** -- including that nothing on
  it is a glyph `term` cannot draw. Also a CP437 caller and a dumb one,
  as byte streams. Covers configuration, the user record, passwords,
  handles, detection, joining, the sysop, bulletins (new since the last
  call, paging, `.ans`), who's online, the user list with non-ASCII
  handles in aligned columns, injection through caller text, profiles,
  timeouts, every node busy, and that no NUL byte is ever sent. And
  messages: posting and the log's format, word wrap and the editor's
  commands, the new-scan's order, last read, replies and quoting, mail
  and who sees it, deleting and who may, forum levels, a bad
  `forums.cfg`, an index rebuilt after a crash, torn writes (including
  one whose length field fits), a forged header, a full editor, an
  editor abandoned by hanging up, and a restart. Run twice: with the
  default sizes (8KB ring, 8KB editor) under AddressSanitizer, leak
  detection and UBSan; then with the Zeitlos sizes (4KB, 6KB).
- `tests/test_zport.c` (21 checks): the real `bbs.c` against a scripted
  kernel: local and network connects, the identity map, "busy", strangers
  by sender and by tag, the eight-send window, a dead peer, the BBS
  ending a call (its words first, then CLOSE), and the caller hanging up.
  Needs `vm.mmap_min_addr=0`; skipped otherwise.

Checked by breaking things on purpose, each of which fails the tests:
matching by tag alone, no dead-peer check, never closing, letting
control characters into fields, running pipe codes in caller text or
in message text, showing a caller someone else's mail, not repairing
the index, appending after a torn write instead of over it, accepting a
torn record whose length fits, letting anyone delete, letting a newline
into a header, and ignoring read levels. (Two of those first survived
-- a torn write left behind, and a torn record accepted -- which is how
the checks on the log's contents and the stricter repair came about.)

The Linux server was also run end to end: telnet (option negotiation,
CR LF / CR NUL / LF as Enter), `--connect` through a real pty both as a
local and an SSH caller (the address from `SSH_CONNECTION` in the log),
a caller dropping, and a clean stop on SIGTERM. For messaging: a post
over telnet, found by the new-scan over `--connect`, answered there
with a quote, and the answer read back over telnet.
