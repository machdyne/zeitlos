# irc

An IRC client. `sw/apps/irc`.

```
> run wm
> run irc
```

Or from the dock (two speech bubbles). With the default configuration
it connects to Libera.Chat at start; set the three keys below to go
elsewhere, join channels automatically, or not connect until asked.

## Using it

One window: the conversation, a status line, and a line to type in.

Anything typed that does not start with `/` is said in the current
channel or conversation. Commands:

| command | does |
|---|---|
| `/connect [host [port]]` | connect (or reconnect); without arguments, to the configured server |
| `/join #channel` | join, and switch to it; `/join zeitlos` means `#zeitlos` |
| `/part [#channel] [reason]` | leave the current channel, or the one named |
| `/msg nick text` | a private message; the conversation gets its own view |
| `/query nick` | switch to a private conversation without saying anything |
| `/me action` | `* zed waves` |
| `/notice nick text` | a notice |
| `/nick newnick` | change nick |
| `/topic [text]` | show or set the current channel's topic |
| `/names [#channel]` | who is in a channel |
| `/whois nick` | about someone |
| `/raw LINE` | send a line to the server as it is |
| `/quit [reason]` | disconnect |
| `/help` | this list |

`//text` says `/text` literally.

### Views

Every channel and every private conversation has its own view, and the
server has one more for everything that belongs to no channel: the
message of the day, errors, notices, people quitting.

| key | |
|---|---|
| Ctrl+N, Ctrl+P | next and previous view |
| PageUp, PageDown, the wheel | scroll back and forward |
| Enter | send the line |

The status line says which view this is, your nick and server, and
lists the other views that have something new: `#fpga*` for new lines,
`alice!` when someone said your nick. A line that mentions you is drawn
in reverse.

Scrolling back keeps your place while new lines arrive, and the status
line says `[scrolled]` until you return to the bottom; sending a line
returns there. The scrollback is 32KB of text shared by all views, the
oldest lines going first. Lines are wrapped when drawn, so resizing the
window reflows everything.

Each line is stamped with the time in `system.rtc.timezone`, once the
clock has been set (NTP, [rtc.md](rtc.md)).

## Configuration

In `/sys/zeitlos.cfg` ([config.md](config.md)):

| key | default | |
|---|---|---|
| `apps.irc.server` | `irc.libera.chat 6667` | host and port to connect to at start; empty: wait for `/connect` |
| `apps.irc.nick` | `zeitlos` | your nick |
| `apps.irc.channels` | *(none)* | joined once connected: `#zeitlos #fpga` |

```
apps.irc.server: irc.libera.chat 6667
apps.irc.nick: zed
apps.irc.channels: #zeitlos #machdyne
```

If the nick is taken while connecting, `_` is added and it tries again.

## How it works

The protocol is `irc_core.c`, which has no I/O: parsing a server line,
deciding what it means (what to show where, what to answer), and
turning what was typed into a line for the server.
`sw/apps/irc/tests/test_irc.c` runs it on the build machine.
`irc.c` is the window, the scrollback and the connection around it.

The connection is a raw TCP socket through `net`, the same kind the web
browser uses. `net` used to keep one such socket, and a chat client
holds its connection for hours, so it now keeps two:
[networking.md](networking.md#more-than-one-socket). Browsing works
while `irc` is connected.

What the client does without being asked:

- **PING is answered** with PONG, which is what keeps a connection
  open.
- **CTCP VERSION and PING are answered**, since those are how other
  clients see what they are talking to and how far away it is. Nothing
  else is: automatic CTCP replies are a well-known way to get a client
  flooded off a network.
- **mIRC colour and formatting codes are removed.** A 1bpp screen can
  show none of them, and left in they draw as garbage.
- **A typed line with a line break in it is refused**, since the rest
  would reach the server as a second command.

Outgoing lines queue while `net` has as many of this client's sends
outstanding as a port allows, and move on each acknowledgement. The
screen redraws once per batch of arriving messages, not once per line,
and the app sleeps between messages rather than polling
([app_runtime.md](app_runtime.md), "Idling").

## Testing

```
make -C sw/apps/irc test      # the protocol, 70 checks
make -C sw/apps/irc render    # the window, drawn by the real repaint()
```

`test_irc.c` covers parsing (message tags, prefixes, trailing
parameters, a server name that is not a nick), every kind of line shown
and where it goes, mentions (including at the very end of a line, which
the first version missed), CTCP, colour stripping, IRC's case folding
(`[]\~` are the upper case of `{}|^`), every command, and the line-break
refusal.

`make render` builds the real `repaint()` against
[`sw/common/tests/zrender.h`](window_manager.md) with a populated
conversation, and writes `/tmp/irc.pbm`. Look at it before changing the
layout.

## Limits

- **No TLS.** Port 6667, in the clear. The TLS 1.3 client is in
  [`web`](web_app.md) and is not shared; a server that only accepts TLS
  (6697) cannot be used. Anything typed, passwords to NickServ
  included, crosses the network readable.
- **One server at a time.**
- **No nick completion, logging, or ignore list.**
- **Twelve views**; a thirteenth conversation shows in the server view.
- **Wide characters** (Japanese) are counted as one column when
  wrapping, so a line of them wraps late.
