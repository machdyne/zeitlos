# zfed: signed messages between independent nodes

*The Zeitlos Federation Protocol, and `fed`, the program that speaks it.*

**Abstract.** zfed carries signed, immutable messages between computers
that each choose their own peers, with no master node and no always-on
server. A network is defined by a signed list of its members; anything
from outside it is refused before it costs more than a few bytes. Nodes
exchange what the other lacks when they meet, by pulling, so a machine
that is off for a week, or connects out only when it can, loses
nothing. Forum posts, private mail sealed to the recipient's node, and
cancels ride on it, and so can anything else small and signed. It is
built for small machines -- an FPGA board with a 48 MHz RISC-V CPU and
hardware assists for its cryptography -- as well as for Linux servers,
and its encryption is hybrid, classical and post-quantum together.

**Status.** Implemented and tested on Linux and on Zeitlos; the BBS
([bbs.md](bbs.md)) uses it for forums, mail between nodes and
moderation. First light on hardware: a Zeitlos board and a Linux server
exchanging objects. Deployment on a live server is next. Implementation
details, measurements and tests are at the end ("Implementation").

---

## 1. Why

Bulletin boards once federated over FidoNet: store and forward, dial
up at night, a hierarchy of hubs assigning addresses. Its virtues were
real -- a node that is offline most of the time still takes part -- and
so were its costs: a hierarchy to be admitted to, and a network whose
shape someone else decides.

Today's federated systems assume the opposite: an always-on HTTPS
server, pushing to others as things happen. That excludes the machines
this protocol is for -- a board on a desk, a machine behind a NAT that
only connects out, a node reachable over a slow radio link, or not at
all for days.

zfed keeps FidoNet's tolerance of absence and drops its hierarchy:

- **No master.** Each node chooses its peers. Any shape of peering -- a
  star, a ring, a mesh -- works, without loops or duplicates.
- **Absence is normal.** A node pulls what it missed when it next meets
  a peer, from where it left off. Nothing is lost by being away; a
  cut-off session costs a repeat, never a gap.
- **Membership is explicit.** A network is a signed list. Spam from
  outside it has nowhere to land, and a stranger costs a node a few
  hundred bytes, not seconds of CPU.
- **Small machines are first-class.** Every cost is measured on a 48 MHz
  board; checks come cheapest first; memory is static and bounded.
- **Content-agnostic.** zfed stores, verifies and passes on objects. It
  does not interpret posts, letters or league tables; applications do.

## 2. Overview

| | |
|---|---|
| **node** | one running `fed`, with one Ed25519 key pair. A BBS is usually one node |
| **network** | a set of nodes defined by a signed **node list**. A node may belong to several |
| **topic** | where an object belongs: `timeless/forum/general`. Nodes subscribe to topics |
| **object** | one signed thing on a topic: a post, a letter, a cancel, a node list. Immutable; named by the hash of its bytes |
| **origin** | the node that made and signed an object. Any node can relay it; none can change it |
| **peer** | a node this node exchanges objects with directly. A node needs only one |

```
   apps (bbs, ...)                apps
        |  local interface          |
      [fed]  <-- session -->      [fed]  <-- session -->  [fed]
        |                           |                       |
      store                       store                   store
```

An application hands its node an object to publish. The node signs it
and stores it. When two nodes next hold a **session**, each pulls from
the other what it wants and lacks -- the objects since its cursor into
the other's store, on the topics it subscribes to. What a node stores it
also relays: an object taken from one peer goes to the others when they
next pull. Because an object's name is the hash of its bytes, one that
reaches a node by two routes is stored once.

## 3. Identity

**A node is an Ed25519 key pair.** The public key *is* the node's
identity; a host name is only a hint about where to reach it. On
Zeitlos the 32-byte seed lives in the flash key/value store, made from
the hardware random source on first start (and `fed` will not make one
without a seeded one); on Linux, a file readable only by its owner.

A node's **short id** is the first 16 hex digits of SHA-256 of its
public key, for logs, screens and topic names. Protocol fields carry
the full key.

**Names are for people.** A node is shown by the name its network's
node list gives it, and a user as `handle@name`. Nothing in the
protocol trusts a name; keys decide everything.

### Names

**One network, one list.** A network has exactly one publisher key, and
its list is a state object: the newest (time, then id) replaces the
older, by the same rule on every node. So every member converges on the
same list; while a new one spreads, some nodes briefly have the one
before. No one but the publisher can bind a name to a key -- a list from
any other key is ignored -- and a list naming two nodes the same is
refused whole. A publisher should never give a name to a different key
later: letters addressed by name while the change spreads would reach
the old holder.

**Several networks, several lists.** A node may follow networks with
different publishers -- but not two with the same *network* name: topics
are named by the network, so the two would compete for the same topics,
and `fed.cfg` refuses it. If two networks disagree about a name, a
sysop follows one. A node name in one list says nothing about the same
name in another. So a name is resolved **within a network**: a
letter goes to `anna@beta` *in that network* -- the local interface
takes `beta@timeless` -- and a bare name that two lists give to
different nodes is refused as ambiguous, never guessed. The BBS always
names the network. Shown names likewise come from the BBS's own
network's list.

### Two people called phil

A handle is unique **on its node**: the BBS refuses a second account
with a name already taken. A node name is unique **in its network**: a
node list that names two nodes the same is refused whole. So
`phil@alpha` and `phil@beta` are two people, told apart by the node,
and neither can be the other:

- Posts carry the writer's handle in their payload, but the node part
  comes from the object's **origin** -- its signing key -- looked up in
  the node list. A node cannot claim another's name; it can only sign
  with its own key.
- A handle arriving from elsewhere has any `@` in it replaced, so no one
  can write as `anna@beta` from alpha: it shows as `anna_beta@alpha`.
- A node the list does not name is shown by its short id, not a name it
  chose for itself.

### One person, several nodes

An address is `handle@node`, and a node vouches only for its own users.
Phil posting on bbs.machdyne.com is `phil@machdyne`; posting from his
own BBS, `phil@mybbs`. Readers who know him know both are him -- and
`phil@someotherbbs` makes no claim to be. There is deliberately no way
for one node to post as another's user: no user keys, no delegations,
nothing to misuse. Identity stays where the node list puts it, on
nodes.

## 4. Objects

An object is bytes, signed as they are and **never re-encoded**: a node
stores and forwards exactly what it received. Text headers, the payload,
then the signature:

```
ZFED1
topic: timeless/forum/general
type: bbs.post
format: json
kind: log
origin: 6a1f...(64 hex: the origin's public key)
time: 1790505386
seq: 1042
len: 184

<184 bytes of payload>
sig: 9c0e...(128 hex)
```

- **The id** is SHA-256 of everything from the magic through the
  payload -- everything but the signature. Identical content signed
  twice is one object (Ed25519 is deterministic), and nothing a relay
  does to a signature changes what an object is called.
- **The signature** is Ed25519 over `zfed object\0` followed by the
  32-byte id -- the id, not the object. Ed25519 hashes what it signs
  with SHA-512, whose 64-bit arithmetic is slow on a 32-bit CPU; signing
  the id makes a check cost the same for a one-line post and a 16 KB
  one. The prefix keeps a zfed signature from meaning anything anywhere
  else.
- `time` is the origin's clock: **Unix time, whole seconds since
  1970-01-01 00:00 UTC**, in decimal. An object more than a day ahead of
  this node's clock is refused. `seq` counts the origin's objects, so a
  node can notice gaps.
- A payload is **at most 16 KB**; the header at most 1 KB. Anything
  larger does not belong on the network ("Constrained links").

The exact byte layout is normative, since signatures cover bytes
(Appendix A, "Encoding, exactly").

### Payloads

The protocol carries payloads; it does not define them. `format:` says
how one is encoded -- `json`, `text` (UTF-8) or `bytes` (anything; a
sealed letter) -- and `type:` what it means (`bbs.post`, `fed.nodes`).

Where `fed` reads a payload itself -- node lists, node info, cancels --
it accepts a **strict subset of JSON**, because two parsers reading one
signed object differently is how signature checks get bypassed: UTF-8
only, no duplicate keys (compared after unescaping), at most 256 keys an
object, integers only within ±2^53, nesting at most 16 deep. Anything
else is refused, never repaired. Consumers parse their own JSON with the
same parser (`zjson`), so every node reaches the same verdict.

### Log and state

- **log** objects are kept, each one, until retention expires: posts,
  letters, cancels.
- **state** objects carry a `key:` and replace each other: for one
  topic, origin and key only the newest is kept (highest time, then
  highest id). A node list, node info. Only an origin can
  replace its own state. A state object lives as long as it is current,
  however old.

## 5. Topics

Names are paths; the first segment says whose they are:

| topic | kind | |
|---|---|---|
| `<network>/nodes` | state | the network's node list |
| `<network>/forum/<tag>` | log | a forum |
| `<network>/mail/<short id>` | log | letters for one node, sealed to it |
| `<network>/cancel` | log | cancels |
| `fed/node/<short id>` | state | a node's info, published by that node only |

Scoping by network means two networks that share a node do not merge
their forums by accident. **Patterns** select topics: a literal path,
or one ending `/*` for everything below it -- nothing else.

## 6. Networks

A **node list** is a state object on `<network>/nodes`, published by
one key, the network's **publisher**. It is the whitelist:

```json
{"network": "timeless",
 "nodes": [{"key": "6a1f...", "name": "machdyne", "sysop": "phil", "addr": "bbs.machdyne.com:9070"}],
 "moderators": [{"key": "6a1f...", "topics": ["timeless/forum/*"]}]}
```

Each entry is a member's public key; name, sysop and an address where
it takes connections are optional. The address is optional on purpose:
a machine behind a NAT is on the list by its key alone and simply
connects out.

**Trust is per network, chosen by each sysop.** A node's configuration
names the networks it takes part in, each with the one publisher key it
accepts that network's list from.

**Who is accepted:**

| objects on | from |
|---|---|
| `<network>/nodes` | that network's publisher only |
| `<network>/...` | that network's members (and this node, and peers configured by hand) |
| `fed/node/<id>` | the node with that short id, if a member of any network here |
| `fed/...`, anything else | a member of any network this node is in |

A sysop's `block:` overrides all of it. A connection is accepted from
the same keys, and, where the list gives a member an address, only from
that address. **Anything wrong in a list -- a duplicate key or name, a
bad field, the wrong network, a malformed moderator -- and the whole
list is refused**, and the previous one stands: guessing what a broken
list meant would let nodes disagree about who is in.

### Joining

A key proves only that whoever sent something holds it. It cannot prove
who they are; that has to come from somewhere the publisher already
trusts. So joining is not a protocol message:

1. The new node's sysop runs `fed --join-request`, which makes the key
   and prints a small signed request (name, sysop, address) and its
   **fingerprint** -- the short id, in groups, like an SSH host key's.
2. They hand it to the publisher over a channel the publisher trusts --
   for Machdyne, naturally, the BBS itself, logged in with an existing
   account -- and the publisher compares fingerprints.
3. The publisher checks it (`fed --check-join`), adds the key, and
   publishes the next list (`fed --publish-list`, which checks it
   exactly as members will). Every member takes it at its next session.

The request itself never goes onto the network: nothing stores or
relays `fed/join`.

## 7. Sessions

A session is one TCP connection, **both directions over it**: whichever
side connected, each then pulls from the other. A node that only
connects out -- behind a NAT, or dialing up when it can -- both sends and
receives every time.

**The handshake** is mutual and encrypted, in the manner of SSH and
Noise, with node keys as the long-term identities -- and **hybrid**:

1. The connecting node sends its Ed25519 key, a fresh X25519 key and a
   fresh ML-KEM-768 key. **The listening node decides from the first 37
   bytes** -- the magic and the Ed25519 key -- before any cryptography:
   a key it will not talk to gets no reply at all. A stranger costs it a
   read.
2. The listener answers with its own X25519 key and an ML-KEM
   ciphertext. Both derive the session keys from **both** secrets: a
   recording stays safe while either X25519 or ML-KEM holds -- against a
   quantum computer included.
3. Each signs the transcript with its node key, with a role byte so a
   signature cannot be replayed in the other role.
4. Frames: XChaCha20-Poly1305, a counter per direction as the nonce.
   Anything that fails to decrypt ends the session.

Then **HELLO** (the store's epoch and the topics wanted), **GET** (after
my cursor into your store), a stream of **OBJ**, **END** (the position
served up to), **BYE**. The receiver checks each object cheapest first
-- the rules, wanted, its origin a member, already here, then the
signature -- and **records its cursor only once what it stored is
durable**. So a session cut anywhere costs a repeat, never a gap.
Nothing echoes: an object is never sent back to the peer it came from.
A peer whose store was lost starts a new **epoch**, and is sent
everything again; ids weed out what it has.

Handshakes are rate-limited per key and per address, doubling up to
ten minutes. Message formats: Appendix B, "Sessions, exactly".

## 8. Storage

`fed` is its store's only writer. Objects are appended to a log in
arrival order, each at a **position**, in segments of 1 MB or 30 days;
beside it, an id index, a state table, and per-peer cursors. Writes go
log first, tables after; a torn record at the end is written over, and
tables behind the log are brought up to it at start. Every crash this
claims to survive is made to happen in the tests.

### Retention

Retention is each node's own setting, not protocol: 90 days by default,
per topic pattern. Whole segments are dropped once everything in them
has expired; current state objects in them are first copied forward, so
state lives as long as it is current. An object already expired when it
arrives is not stored: a lagging peer cannot bring deleted history back.

## 9. Mail between nodes

A letter to `anna@lakeside` is a log object on
`<network>/mail/<lakeside's short id>`, its payload **sealed to
Lakeside's mail key**. It floods like anything else -- any node may
carry it, only Lakeside can read it -- and it is signed by the sending
node, so Lakeside knows where it came from.

**Mail keys** are derived from the node's seed: an X25519 key and an
ML-KEM-768 key pair, by HMAC-SHA256 with fixed labels. Nothing more to
store or to lose; whoever has the seed has the node already. Each node
publishes its public mail keys in its **node info**
(`fed/node/<short id>`), which only that node may publish -- otherwise
anyone could plant a key of their own for someone and read their mail.

**Sealing** is a hybrid KEM, so that breaking either algorithm alone
reads nothing: an ephemeral X25519 exchange and an ML-KEM encapsulation,
both secrets into HMAC-SHA256 with the ephemeral key and the recipient's
X25519 key bound in (as X-Wing combines them), then XChaCha20-Poly1305
with the header authenticated. The cost: 1,164 bytes on top of the
letter. The recipient's ML-KEM key must pass FIPS 203's input check
first.

**What is visible**: who writes to which node, when, and about how
much, to every node that carries the mail topic. What is written is not.

**Applications never see a mail key**: they ask their node to `MAIL` a
letter to a node (by key, short id or name) and to `OPEN` one; `fed`
does the cryptography.

## 10. Moderation

Local first: a sysop can block an origin outright, and a BBS sysop can
delete anything on their own node. Beyond that, **cancels**:

- A cancel is a log object on `<network>/cancel`:
  `{"id": "<the target's id>", "topic": "<its topic>"}`. It is stored
  and relayed like anything else.
- It is **honoured** when it comes from the target's origin, or from a
  **moderator** the network's list names for the target's topic --
  judged by the target's own topic, never by what the cancel says.
- An honoured cancel marks its target **cancelled**: not delivered to
  applications or relayed again. A target that has not arrived yet --
  flooding brings things in any order -- is **refused when it does**.
- The network knows nodes, not users: "the origin" is the writer's
  node, and which of its users may cancel is that node's business (the
  BBS: the writer, or its sysop). A BBS asks its node whether a cancel
  was honoured before deleting a message.

## 11. Constrained links

LoRa matters as a fallback -- if internet access became unavailable --
and for places without it: a remote village, a camp. Day to day it is
unlikely to carry much. The protocol assumes TCP today; this section is
how it reaches a radio link, alone or **mixed** with the internet. The
first part is built; the rest is proposed, and waits for the radio
transport itself (`mesh`).

**The problem, in numbers.** A LoRa packet carries roughly 50-250
bytes, at hundreds to a few thousand bits a second, and the law often
limits a transmitter to 1% of the time -- about 36 seconds an hour.
Over that, zfed's usual sizes dominate:

| | over TCP | on radio |
|---|---|---|
| an object's envelope | ~350 bytes (the origin and signature in hex, text headers) | more than most posts |
| a session handshake | ~2.4 KB each way (ML-KEM) | tens of packets before any content |
| a sealed letter | letter + 1,164 bytes | a one-line letter, eight packets |
| a node's full info | ~2.6 KB (its ML-KEM key) | minutes of airtime |
| the largest payload | 16 KB | not at all |

### Network profiles

*Built.* A node list declares its network's limits, and every member
enforces them:

```json
"profile": {"max_object": 1024, "suite": "classical"}
```

- **`max_object`** (512 bytes up to the largest object): larger objects
  on the network's topics are refused -- when published, and on arrival.
  The list itself is exempt; its publisher decides its size.
- **`suite`**: `hybrid` (the default) or `classical` -- how letters on
  the network are sealed. `classical` is X25519 alone: **76 bytes** of
  overhead, not 1,164 (Appendix B, `ZMX1`) -- and no protection against a
  future quantum computer, which is the network's choice to make,
  written in its list.
- Without a profile, nothing changes.

### Links: a mixed network

*Built.* A network that is partly internet and partly radio is **one
network with some small links in it**, not two networks joined by
gateways. A link's limit is set per peer, in `fed.cfg`:

```
peer: 6a1f... max=700
```

Objects larger than that are **not sent over that link** (the sender
withholds them) and **refused from it** (the receiver checks too), so
the limit holds whichever end is configured. Node lists are exempt.
Large posts stay on the internet side; small ones reach the radio side
-- only smaller messages get through, by design, with no gateway
machinery and no trusted relays. A node that is on both is simply a
peer over both links.

Mail keys have to cross such a link too: each node publishes its info
twice -- the full form (2.6 KB, with its ML-KEM key) and a **small form**
(~450 bytes: its name and X25519 key), each on `fed/node/<short id>`
under its own key. A letter on a classical network needs only the
small one. On hybrid networks nothing changes.

### Membership changes

*Built; found by the mixed-network test.* An object whose origin is not
yet known to be a member -- because the list saying so has not arrived
-- is refused, and the receiving node's cursor moves past it. Without
more, it would be lost on that path for good: most exposed, a node
joining a network, whose first sessions pull everything before its list
has settled. So **a new node list clears every cursor**: the next
sessions ask from the start, and what was refused is taken now. What a
node already has costs an id lookup, before any signature check; lists
change rarely. A session a list arrives in has its peer's cursor cleared
again when it ends, since its own END would otherwise move it past what
it refused.

### The radio: Meshtastic, through `mesh`

Zeitlos already has a radio transport: the `mesh` app
([mesh_app.md](mesh_app.md)), a client for a LoRa node running
Meshtastic firmware. **The firmware does the radio work**: routing,
retransmission, the duty cycle, and **channel encryption** -- AES with
the channel's pre-shared key. A client hands it plaintext and receives
plaintext.

**`mesh` stays the only program that speaks Meshtastic** -- the USB
serial device, the framing, the protobufs. `fed` never sees them: it
exchanges bytes with `mesh` through a small service port (proposed:
`mesh0` -- "send these bytes on private application port N", "these
arrived on it"; Meshtastic reserves ports 256 and up for applications),
and runs its radio link (below) over that. So **LoRa is Zeitlos only**:
a Linux node would need an adapter speaking the same small port
protocol, which is not provided.

**Packets are small** -- about 230 bytes of payload on Meshtastic, less
at the slowest radio settings -- and an object is usually several
hundred, so **objects are split into fragments and put back together**.
Yes, that is a protocol of its own, but a thin one *below* zfed, as TCP
is below it over the internet: a fragment carries the object's id
(its first 8 bytes), its index and the count; the receiver collects
them, rebuilds the object, and checks it against its id -- SHA-256
covers the reassembly, so no checksum of its own is needed -- and asks
for the fragments it lacks. zfed above it does not change: objects,
signatures and ids are exactly those on the internet.

**What the radio does to security -- and who decides.** A network's
publisher does not know which of its members carry its traffic over a
radio, or how; a member sysop can put it on a link the publisher never
sees. So what matters is what stays in the publisher's hands:

- **Letters: the publisher decides, and no one downstream can weaken
  them.** The sealing suite is in the signed list, not in any node's
  configuration, and objects cross every link **unchanged** -- nothing
  is ever re-sealed. A hybrid letter carried over a radio by a member's
  BBS is still hybrid; someone recording the broadcast, with a quantum
  computer or without, still cannot open it.
- **Forums: no one ever could decide.** Posts are signed, not encrypted,
  and every member has them; a member can republish them anywhere. A
  radio link adds one more way out -- a channel with a weak or public
  key -- but the boundary was always membership, not the transport.
- **The link itself: the local operators decide**, not the publisher.

On the link:

- **For the main network, assume forums are public -- possibly
  quantum-leaked**: with many independent nodes, one will carry them
  over a weak link sooner or later. Letters, sealed hybrid end to end,
  are the confidential channel.
- The Meshtastic channel's AES key is **symmetric** -- a quantum computer
  only halves its strength -- so a **private channel with a random
  256-bit key** protects the radio link against outsiders, now and
  later. The default channels' keys are public: a zfed channel must have
  its own.
- Everyone who has the channel key hears everything on it. For forums
  that changes nothing -- members see them anyway. Letters stay sealed to
  their node.
- **`suite: classical` is network-wide**: it weakens every letter on
  the network, including ones that never touch the radio, and recording
  a broadcast is trivial. So a **mixed** network should keep `hybrid`
  and pay for its letters in airtime -- 1,164 bytes, about six packets
  -- and `classical` is for networks that are radio-only, or nearly.

### The radio link

*Built; tested on the host, not yet on the air.* `core/fradio.c`: zfed
objects over a broadcast medium of 233-byte packets. It knows nothing of
Meshtastic or Zeitlos -- the platform sends what it gives it and hands
it what arrives; on Zeitlos, `fed` is a client of `mesh0`. Three
packets, an id being the store's own 16-byte prefix of an object's:

```
'I' n(1) n x id(16)                                     I have these (at most 14)
'W' id(16) bitmap(0..32)                                these fragments, please (none: all)
'F' id(16) len(2) index(1) count(1) data(<= 212)        a fragment
```

- **Announcing**: what arrives here is announced -- **never what came by
  radio**: Meshtastic carries packets across hops itself, and echoing
  them would loop. The newest are announced again now and then, for
  nodes that were out of range. An object over the link's limit is
  never announced: asking for what cannot come would waste the air.
- **Asking, and serving**: a node wants what it lacks; **fragments are
  broadcast**, and a node collects those it overhears for anything it
  lacks -- one transmission serves every listener.
- **Putting back together**: an object is checked against its id first
  -- a fragment damaged on the way fails it, and it is wanted again --
  then exactly as a session checks one: wanted, its origin a member, not
  cancelled, its signature. Missing fragments are asked for again, a few
  times, then given up.
- **Pacing**: at most one packet each `pace` milliseconds; the air is
  the scarce thing.
- At most **4 KB** an object over the radio -- 20 fragments -- and two
  put back together at once: ~12 KB of memory.

```
radio: port=300 channel=1 max=2048 pace=2000     # fed.cfg -- Zeitlos only
```

The Meshtastic channel should be a private one, with its own random
key (above).

### What remains, for the radio transport

*Proposed.*

- **A compact air encoding of the same objects**: on a radio link the
  origin travels as its index in the node list (one or two bytes, not
  64 hex digits), the signature as 64 raw bytes, time and sequence as
  variable-length integers, topic, type and format as codes. The
  receiver **rebuilds the exact canonical text** and checks the
  signature against that -- an object carried by radio is the same
  object, with the same id; nothing is ever re-signed. A 200-byte post
  would go from ~550 bytes to ~290.
- **Classical sessions** for `classical` networks: link encryption from
  X25519 alone (64 bytes, not 2.4 KB).
- **Priorities** on the air -- letters and short posts before anything
  else -- and catching up older history over the radio (today it carries
  what is new, and the newest again).
- **A gateway to separate networks**, where one is wanted: into the
  mesh, only objects within its limit; for bridged topics the gateway is
  a trusted relay, since the mesh cannot carry a large internet
  network's list. For most uses the mixed network above makes this
  unnecessary.

## 12. Security considerations

**What is protected.**
- **Integrity and origin of every object**: signed by its origin's key;
  nothing a relay does can change it without detection.
- **Membership**: nothing from outside a network's list is stored or
  relayed, and a connection from a stranger ends before any cryptography.
- **Sessions** are confidential and authenticated, hybrid classical and
  post-quantum.
- **Letters** are readable only by the node they are sealed to.

**What is not.**
- **Forums are public to the network**: every member stores them.
- **Metadata**: who writes to which node, when and how much; which
  listed node is connecting (its key is sent in the clear, so a stranger
  can be refused cheaply).
- **A member node can publish anything** on its network's topics. The
  answers are moderation, blocking, and taking it off the list.
- **A node's own compromise** exposes its letters and lets its key sign.
- **The publisher is the network's root of trust**: whoever holds its
  key decides membership. It should be kept offline, and used only to
  sign lists.

**Denial of service.** A stranger costs a node 37 bytes read. Handshakes
are rate-limited per key and per address. Every object is checked
cheapest first, the signature last; sizes are bounded everywhere; the
JSON `fed` reads has bounded keys and depth; any malformed input is
refused, never repaired.

**Clocks.** An object's time is its origin's claim, bounded only a day
into the future. It orders state objects and drives retention; nothing
security-relevant depends on it being true.

### Quantum computers

A large enough quantum computer would break Ed25519 and X25519 (Shor's
algorithm recovers a private key from its public key, and zfed's are
public, in node lists); it would only weaken SHA-256 and
XChaCha20-Poly1305, leaving ample margin, so **object ids stay sound**.
NIST's draft transition plan deprecates elliptic-curve signatures after
2030 and disallows them after 2035.

- **Encryption is the risk today** -- a session recorded now could be
  decrypted then -- so sessions and sealed letters are **hybrid from the
  start**: X25519 and ML-KEM-768 together, safe if either holds, as
  OpenSSH now does by default.
- **Signatures can wait, but not past a deadline.** The plan: each node
  publishes a post-quantum key (ML-DSA, FIPS 204) in its node info,
  signed by its Ed25519 key, **while Ed25519 is still trustworthy** --
  done late, the binding means nothing and every node must re-join by
  hand. Then a new magic, `ZFED2`, with that signature; ids do not
  change. The cutover goes by an object's arrival, not its claimed
  time, which a forger chooses.
- The cost to expect: an ML-DSA-44 signature is ~2.4 KB against
  Ed25519's 64 bytes -- small objects roughly triple, which is one more
  reason constrained networks will choose their suite.

## 13. Limits, and what is next

- **Built**: objects, networks and joining, hybrid sessions, the store,
  the local interface, mail between nodes, cancels and moderation,
  network profiles and link limits -- on Linux and on Zeitlos; the
  radio link, through `mesh0` -- on Zeitlos.
- **Next**: **events** -- signed announcements to the OS itself, a
  Zeitlos update for example, from a publisher the network names, as
  node lists are; the radio link on the air (built and host-tested:
  section 11), then its compact encoding and classical sessions; an
  optional FTN gateway.
- **Not planned: files.** Neither file areas in the BBS nor files over
  zfed: too big for radio, and the web, FTP and the like move files
  better. Other networks could want them one day -- then as an option,
  off by default, size-limited, and not on ours.
- Remembered cancels (for targets not yet arrived) do not survive a
  restart; a Zeitlos node checks addresses in lists only when they are
  literal IP addresses.

---

## Appendix A. Encoding, exactly

Signatures cover bytes, so the bytes are pinned down; `fobj.c` and the
independent `tests/fobj_ref.py` implement exactly this.

```
ZFED1\n
topic: <topic>\n
type: <type>\n
format: json | text | bytes\n
kind: log | state\n
origin: <64 lowercase hex>\n
time: <decimal>\n
seq: <decimal>\n
key: <key>\n              state objects only
len: <decimal>\n
\n
<len bytes of payload>
\nsig: <128 lowercase hex>\n
```

- Lines end in `\n` alone; after each `:` exactly one space; values are
  printable ASCII without spaces.
- Fields in exactly this order; any other field, a missing one, or
  `key:` on a log object is refused. Unknown fields are refused in
  version 1, so a later version says so with a new magic.
- `topic` 1-96 bytes, `key` 1-64, both `a-z 0-9 / . _ -` with no empty
  segment; `type` 1-64 bytes of `a-z 0-9 . _ -`.
- `time`, `seq`, `len`: decimal, no sign, no leading zero, at most
  2^53; `seq` at least 1; `len` at most 16,384. The header, magic
  through blank line, at most 1,024 bytes.
- The **id** is SHA-256 of everything from `Z` through the payload's
  last byte; the signature is Ed25519 over `zfed object\0` (12 bytes)
  and the 32-byte id.
- Where `fed` reads a payload, JSON is its strict subset (section 4).

## Appendix B. Sessions, exactly

I is the connecting node, R the listening one.

```
I -> R  clear   "ZFED1" | I's Ed25519 key | I's X25519 key | I's ML-KEM-768 key    1253 bytes
R -> I  clear   "ZFED1" | R's X25519 key | ML-KEM ciphertext                        1125 bytes
        th   = SHA-256("zfed handshake 1" | the 1253 | the 1125)
        prk  = HMAC-SHA-256(key th, X25519(ours, theirs) | the ML-KEM secret)
                                                -- X25519 all zeros: no session
        i2r  = HMAC-SHA-256(prk, "zfed i2r" | 0x01)
        r2i  = HMAC-SHA-256(prk, "zfed r2i" | 0x01)
R -> I  AUTH    R's Ed25519 key | sign("zfed session\0" | th | "r")
I -> R  AUTH    sign("zfed session\0" | th | "i")
```

R encapsulates to I's ML-KEM key only after FIPS 203's check on it, and
decides on I's key from the first 37 bytes. Every frame after the first
two messages: a 2-byte big-endian length L, then L bytes of
XChaCha20-Poly1305 ciphertext and its tag, the direction's key, a 64-bit
counter as the nonce (little-endian, then zeros), the length bytes as
associated data. At most 20 KB of plaintext; its first byte the type:

| type | | body |
|---|---|---|
| 1 | AUTH | above |
| 2 | HELLO | our store's epoch (8 bytes), the patterns we want (`\n`-separated) |
| 3 | GET | after this position (4 bytes) |
| 4 | OBJ | the object's position (4 bytes), the object |
| 5 | END | the position served up to (4 bytes) |
| 6 | BYE | -- |

A GET is answered up to the position the store had when it arrived. On
END: the store made durable, then the cursor recorded, then BYE. A frame
that does not decrypt, a message out of place, 30 s without finishing
the handshake, or 60 s with nothing arriving ends the session; the
cursor stays where it was. Reserved for constrained links: INV and WANT
(section 11).

**Sealed letters** (format `bytes`), classical (`suite: classical`,
section 11):

```
"ZMX1" | ephemeral X25519 key (32) | nonce (24) | XChaCha20-Poly1305(the letter) | tag (16)
prk  = HMAC-SHA256("zfed mail x 1", X25519(ephemeral, the node's X25519 key))
key  = HMAC-SHA256(prk, "zfed mail key" | ephemeral | the node's X25519 key | 0x01)
```

and hybrid (the default):

```
"ZML1" | ephemeral X25519 key (32) | ML-KEM-768 ciphertext (1088)
       | nonce (24) | XChaCha20-Poly1305(the letter) | tag (16)
ss   = ML-KEM shared secret || X25519(ephemeral, the node's X25519 key)
prk  = HMAC-SHA256("zfed mail 1", ss)
key  = HMAC-SHA256(prk, "zfed mail key" | ephemeral | the node's X25519 key | 0x01)
```

the header (magic, ephemeral key, ciphertext) as associated data. Mail
keys: `prk = HMAC-SHA256("zfed mail keys 1", seed)`; the X25519 secret
`HMAC(prk, "x25519" | 0x01)`; ML-KEM's d and z `HMAC(prk, "ml-kem-768 d"
| 0x01)` and `HMAC(prk, "ml-kem-768 z" | 0x01)`. Node info, key `info`:
`{"name": ..., "mail": {"x25519": "<64 hex>", "mlkem": "<2368 hex>"}}`;
and key `x25519`, the small form: `{"name": ..., "mail": {"x25519": "<64 hex>"}}`.

## The local interface

Applications do not speak the protocol; they talk to their own node's
`fed`, in text lines -- the port `fed0` on Zeitlos, a Unix socket
(`<dir>/fed.sock`, mode 0600) on Linux, the same lines on both:

```
PUB <topic> <type> <format> <kind> <key or -> <len>\n  then the payload
    -> OK <id> <position>\n | ERR <why>\n
SUB <name> <pattern>\n
    -> OK subscribed after <position>\n
    -> OBJ <position> <len>\n then the object -- one at a time, the next after
ACK <position>\n                  also remembered as <name>'s place
KEY\n
    -> OK <this node's key> <its short id> <its name>\n
MAIL <node> <type> <len>\n then the letter -- sealed to that node, published
    <node>: a key, a short id, a name, or name@network (a bare name two
    lists give to different nodes is refused as ambiguous)
    -> OK <id> <position>\n | ERR <why>\n
OPEN <len>\n then a sealed payload
    -> OK <len>\n then the letter | ERR not a letter to this node, or tampered with\n
CANCELLED <id>\n
    -> OK yes | OK no
```

**Delivery is at least once**: an object counts as delivered when its
ACK is read; one delivered before a crash may come again, so consumers
ignore repeats by id. A field too long is refused, never cut short. A
client whose first line ends in `\r` (a person at a terminal: `port
fed0`) gets its typing echoed and `\r\n` line ends; a program gets the
bytes exactly.

## Configuration

`fed.cfg` (on Zeitlos in `/fed`, or `apps.fed.dir`):

```
name: machdyne                     # this node's name
listen: 9070                       # takes connections; omit for outbound only
poll_seconds: 900
network: timeless 3b7c...          # a network, and the key its list comes from
peer: 6a1f... bbs.machdyne.com:9070    # connect to it
peer: 91d2...                      # it connects to us
peer: 5c0b... max=700              # a small link: nothing larger crosses it
member: 5e2a...                    # take its objects, list or not
                                   # (one network per name: two "network:
                                   # timeless" lines are refused)
subscribe: timeless/*               # plus, always: every network's list,
                                   # this node's own mail, fed/node/*
retain: 90d; timeless/door/*=30d
block: 0f3e...
```

---

## Implementation

### Building it

The protocol was specified first, reviewed, and then built in steps,
each with host tests before the next. The code is `sw/apps/fed`:

| | |
|---|---|
| `core/fobj.c` | objects: make, parse, sign, check, id |
| `core/fstore.c` | the store |
| `core/fsess.c` | sessions, the hybrid handshake |
| `core/fnet.c` | node lists and moderators |
| `core/fmail.c` | mail keys, sealing and opening |
| `core/fradio.c` | the radio link: inventory, wants, fragments |
| `core/fnode.c` | the node: configuration, rate limits, sessions, the local interface, mail, cancels |
| `linux/main.c` | the Linux daemon: `poll()`, TCP, the Unix socket |
| `zeitlos/fed.c` | Zeitlos: `net`'s listener and raw sockets, the port `fed0` |
| `sw/common/zjson.c`, `zmlkem.c`, `z25519.c`, `zsha256.c`, `zkeccak.c` | strict JSON, ML-KEM, X25519/Ed25519, SHA-256, Keccak -- with hardware paths |

**Tests** (`make -C sw/apps/fed test`, and `make live`):

| suite | checks | what |
|---|---|---|
| `test_fobj` | 6, over 503 vectors | against an independent Python implementation, both ways |
| `test_fstore` | 59 | every crash the store claims to survive, made to happen; a fuzz against a model; cancels |
| `test_fsess` | 21 | stores syncing both ways; relays; a store lost; a session cut at 46 points; refusals |
| `test_handshake` | 12 | a known-answer test of the whole hybrid handshake against Python |
| `test_fnode` | 48 | configuration, rate limits, membership from real signed lists, the local interface |
| `test_fnet` | 13 | every kind of wrong list refused whole; moderators; profiles |
| `test_fmail` | 19 | sealed letters, hybrid and classical, byte for byte against Python; every byte tampered with; round trips |
| `test_zfed` | 16 | the Zeitlos program against a scripted kernel, a real Linux `fed` over real TCP, and a scripted `mesh0` |
| `live_fed.py` | 24 | two and three daemons: posts both ways, resuming, joining and leaving |
| `live_mail.py` | 22 | node info spreading; a letter only its node can open; one name in two networks |
| `live_cancel.py` | 10 | authors, strangers, moderators; a cancel before its target |
| `live_profile.py` | 16 | a mixed network: the network's limit, link limits at either end, small node info, classical letters |
| `test_fradio` | 22 | the radio link: three nodes and the air -- clean, 30% lost, 10% damaged, a stranger; no echo, nothing too big announced |

Each suite's central claims were also **broken on purpose**, to see the
test fail. Bugs the tests found along the way include: a session failing
at once because an unsigned "now minus start" turned a millisecond early
into four billion; the store's torn-record test passing for the wrong
reason (the shell's `printf` could not write the bytes); a cursor that
ignored epochs, hidden until a restarted peer made new objects; a
BBS notice read from a record that had no terminator; and objects
refused before their origin's node list arrived, lost for good on that
path -- found by the mixed-network test, and the reason a new list now
clears every cursor; a bare node name resolved in whichever of two
networks' lists came first -- a letter could have been sealed to the
wrong node -- now `name@network`, and ambiguity refused; and a node that
followed a network without subscribing to its topics never getting its
list -- now always wanted. One broken check
was found to catch nothing -- the cancel's topic agreeing with its
target's -- because authority never rested on it; it is documented as
the consistency rule it is.

### On Linux

```
fed --dir /var/lib/fed --print-key      # the node's key (made on first run)
fed --dir /var/lib/fed [--bind ADDR]    # DIR/fed.cfg, DIR/node.key (0600), DIR/store
```

### On Zeitlos

`run fed`. It registers `fed0`, keeps its files in `/fed`, reads the
node key from the key/value store (`apps.fed.nodekey`), listens through
`net` and dials its peers through `net`'s raw sockets, one at a time --
every connect and DNS lookup answered in its loop, since blocking
helpers would drop other sessions' messages. It is in the LARGE memory
tier; its image is ~0.98 MB, sized for the 32 MB boards. The address
check in lists knows only literal IP addresses there.

### The board's limits

- **One outbound connection at a time**: two client sockets in the whole
  system, shared with the browser and IRC. Inbound connections count
  against the TCP pool of eight; `fed` takes at most two.
- **About 11 KB/s.** 1,000 typical posts are about 1 MB.
- **Memory is static**: three sessions of ~45 KB, the list parser's
  tokens, the lists, client buffers; nothing kept in memory that the
  store holds.
- **Eight file handles for the whole system**; `fed` never holds more
  than two.

### Measured on the board (Sergei ML2, 48 MHz)

| | software | with the hardware blocks |
|---|---|---|
| Ed25519 check | 714 ms | **131 ms** (montmul's register file) |
| X25519 | 519 ms | **83 ms** |
| ML-KEM-768 keygen / encaps / decaps | 237 / 272 / 347 ms | **114 / 145 / 196 ms** (the Keccak block) |
| a session: initiator / responder | | ~0.6 s / ~0.45 s |
| catching up 1,000 objects (checks) | ~12 min | ~2 min |

### Hardware

Three blocks make the numbers above ([crypto_hw_options.md](crypto_hw_options.md)):

- **SHA-256** ([sha256_hw.md](sha256_hw.md)): 1,413 cycles a 64-byte
  block, 32 times software -- ids, the handshake, HMAC.
- **montmul with a register file** ([montmul.md](montmul.md),
  [z25519.md](z25519.md)): numbers stay inside the block, and software
  issues `MUL`, `ADD` and `SUB` on registers -- a multiply 767 cycles
  against ~8,900 in software. Ed25519 checks and X25519 run on it, with
  Monocypher's exact verdicts; the X25519 ladder swaps register numbers,
  not values, in constant time.
- **Keccak** ([keccak_hw.md](keccak_hw.md)): one round a clock, 31 times
  software for the permutation -- ML-KEM's hashing.

Each is claimed by one process at a time; the kernel clears and
releases a block whose owner dies, so nothing leaks between processes.
With them, the "trusted relay" option (a peer whose objects are stored
without checking signatures, D5) is not needed on boards that have
them; it remains for boards without.
