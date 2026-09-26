# Security: the password and the screen lock

Zeitlos is a single-user system, and this is that user's one password.
It locks the screen, can lock the serial console, and is what telnet
and SSH password logins ask for ([netserve.md](netserve.md)). SSH can
take keys instead, or as well.

| | |
|---|---|
| `sw/os/authcore.c` | PBKDF2-HMAC-SHA256, the stored record, the delay schedule; no platform dependencies |
| `sw/os/auth.c` | the kernel's side: `Z_SYS_AUTH`, `passwd`, `lock`, the console lock |
| `sw/common/zauth.h` | the app API |
| `sw/apps/wm` | the lock screen |
| `sw/apps/settings` | the Security section |
| `sw/os/tests/test_auth.c`, `sw/apps/wm/tests/test_lock.c` | host tests |

**Status: running on a board** -- the password, `passwd`, the settings
pane, and password logins over telnet and SSH. The screen lock and its
idle and boot variants, and the console lock, have not yet been
reported from a board.

## What it protects, and what it does not

Be exact about this, because a lock that promises more than it does is
worse than none.

**It protects the running session from someone at the keyboard.** Open
windows, unsaved work, and the machine's ability to act as you: run
apps, read files through the running system, and -- with `netserve`
exists -- log in over the network. Someone who sits down at an
unattended, locked machine gets a password prompt and a growing delay
after each wrong guess.

**The web server asks for nothing:** it serves one directory,
read-only, and refuses any path that would leave it
([netserve.md](netserve.md), "The web server"). Put nothing there you
would not publish. Like telnet, it answers only this subnet unless told
otherwise.

**SSH** ([netserve.md](netserve.md), "SSH") asks for the same
password, over an encrypted connection with strict key exchange; its
host key lives in the flash store and is exactly as private as the
code this machine runs. Prefer it to telnet anywhere but a bench. With
keys (`/user/authkeys`) it can take no password at all
(`apps.netserve.ssh_auth: key`) -- the setting for a machine reachable
from outside the local network. The key list is on the card, so
whoever can edit the card can add a key: the same boundary as the rest
of this page.

**It protects the machine from the network** ([netserve.md](netserve.md)):
every password login, telnet or SSH, is checked by the same kernel
code, against the same tally of failures. A key login is checked
against `/user/authkeys` instead, and needs no password at all.

**It does not protect what is on the sdcard.** The card is plain FAT.
Pull it, read it in any computer. This is a screen lock on a laptop
without disk encryption, and nothing more. Password-based encryption
is a separate, larger piece of work, not yet designed -- see "Open:
encryption" below.

**It does not protect against code running on the machine.** Any app
can read any address, the flash included ([mpu.md](mpu.md)), so the
password's hash is readable by every app, and apps are trusted to draw
only inside their windows ([window_manager.md](window_manager.md), "App
trust model"). The lock keeps a compliant app off the screen; it does
not contain a hostile one.

**It does not protect against someone with the hardware and time.** A
serial cable reaches `passwd reset`; JTAG or DFU can rewrite the flash;
a card with a changed `wm.bin` on it replaces the lock screen itself.

### Why `wm` is not loaded only from flash

An earlier plan had the kernel load `wm` from flash, never from the
card, while a password is set, so that a changed `wm.bin` on the card
could not remove the lock. It was dropped: `repl`, `posix` and `net`
also come from the card and run with the same access, so it would have
closed one door of several and suggested that tampering with the card
was guarded against. It is not, and the documentation says so instead.

## The password

**Where:** the flash key/value store ([kvstore.md](kvstore.md)), key
`auth.password`, so it holds with no card and does not travel with one.
Apps can see that the key exists but cannot read or write it through
the store's API; only `auth.c` writes it, and only after the current
password has been given.

**What:** never the password itself. A 54-byte record: a version, a
flag, the iteration count, a 16-byte salt and a 32-byte
PBKDF2-HMAC-SHA256 key. Changing the password writes it with
`KV_SCRUB`, so the old record is erased from the flash, not merely
superseded.

**Salt:** 16 bytes, from the TRNG where the board has one
([trng.md](trng.md)), hashed with the tick count and the cycle counter.
A salt has to be unique, not secret; the timers alone would do on a
board without a TRNG.

**Length:** 1 to 64 bytes of printable ASCII. Short passwords are
accepted -- a PIN is reasonable for a screen lock -- but one under
**10 characters** is flagged (`Z_AUTH_NET_OK` clear), and `netserve`
refuses to offer telnet, or SSH password logins, with it. `passwd` and settings
both say so when one is set.

### The hash is cheap here, and that is said plainly

A check costs about **half a second** of CPU: when a password is set,
the kernel times PBKDF2 on this machine and chooses the iteration count
that takes that long. The boot log prints it:

```
 - auth: password set (NNN iterations); lock: boot 1, idle 5 min, console 0
```

On a picorv32 that is a few hundred iterations, where a desktop
guideline is hundreds of thousands. So **a copied hash of a weak
password is cheap to crack** on any PC. Argon2 or scrypt would not
change that much: at half a second of this CPU any function is cheap
somewhere faster, and both need memory the lock screen's process (`wm`,
8 KB) and the kernel (no heap) do not have.

What actually protects the password is that guessing has to go through
the kernel, which is slow and remembers.

### Guessing is slow, everywhere at once

Every check -- the lock screen, the console, settings, and every
password login over telnet or SSH -- goes through one kernel function
with one tally:

| consecutive failures | before the next check is allowed |
|---|---|
| 1-3 | no wait (each check still costs ~0.5 s) |
| 4, 5, 6, 7, 8, 9 | 1, 2, 4, 8, 16, 32 s |
| 10 and after | 60 s |

A success resets it. **One check runs at a time**, system-wide: a
second caller during a check is told to wait, so opening several
connections does not multiply the rate. A process killed mid-check
releases that gate through the reaper, so the lock cannot wedge. Each
failure is logged on the console with the caller's name.

The tally is not kept across reboots: that would cost a flash write per
guess. A power cycle takes longer than the delays it would clear.

## The screen lock

**How it locks:** Super+L, `lock` in the repl or at the console, at
boot (`sys.lock.boot`), or after `sys.lock.idle` minutes with no key or
pointer movement. All of them set a request that `wm`'s main loop acts
on, so a lock never begins in the middle of something else, and one
requested during a window drag waits for the button to be released.

**What locking does**, in this order:

1. An app holding the whole screen (game mode) has it taken back, and
   wm's own game-mode camera is turned off.
2. Every app window is **frozen** -- told its region is empty, as a
   drag does -- and wm waits, up to 300 ms, for each to acknowledge.
   Only then can nothing be mid-frame.
3. Every window, the dock and wm's own chrome get an empty visible
   region. zgfx confines every primitive to the region, so anything
   drawn through it -- by an app, by the dock, by wm repairing a window
   that opened or closed -- lands nowhere.
4. The lock screen is drawn: a box, "Zeitlos is locked", the password
   field, a status line.

**While locked:** every key goes to the field and nowhere else -- no
app, no hotkey. Key combinations with Ctrl, Alt or Super type nothing.
Printable ASCII only, as the console and settings take. Backspace and
Escape edit; Enter checks. Pointer clicks go nowhere. An app that tries
to take the screen is refused at once. Captions stay hidden. The ESP32
remote desktop ([remote_desktop.md](remote_desktop.md)) streams the
framebuffer, so it shows the lock screen too, and its keys arrive
through the same path.

**Checking:** "Checking..." is drawn first, because the kernel spends
half a second on it. The field is wiped after every attempt, right or
wrong. A wrong password says so; a delay says how long.

**Unlocking** gives every window its real region back, then repaints
the whole screen once, which asks every app to redraw.

**With no password set** the lock still works, as a curtain: Enter
opens it. That is also what happens on a kernel without `Z_SYS_AUTH`.

`sw/apps/wm/tests/test_lock.c` holds `wm` to all of this, with the real
`wm.c` drawing into a software framebuffer that enforces regions as the
device does. The check that matters most is that **a full repair and a
caption, drawn while locked, change no pixel** of the lock screen. Each
of the nine properties it checks was confirmed to fail when the code
protecting it was removed.

## The console

```
> passwd
current password:
new password (empty to remove it):
again:
passwd: working...
passwd: password changed
```

Nothing typed is echoed. `lock` locks the screen and, if the console
lock is on, the console as well, until the password is typed at it.
With `sys.lock.console` on, the console also asks for the password
before its first prompt, after init has started the desktop.

The kernel console is one shell reached two ways: UART0 and the
`console0` port in a term window ([console.md](console.md)). The
console lock covers both. **`passwd reset` does not**: it removes the
password without asking for it, so it is accepted only when the whole
command line and its confirmation were typed on UART0. The UART
receive ring marks every byte injected through `console0`
(`k_uart_last_injected`, `sw/os/uart.c`), which is how the kernel can
tell.

## Recovery

- **The password is forgotten, the console lock is off:** `passwd
  reset` at the serial console. It removes the password and the lock
  settings.
- **The console is locked too:** erase the store's two sectors from a
  host (the last 8 KB of the flash, [kvstore.md](kvstore.md)), or flash
  a full or DFU image, which erases them anyway. An empty store means
  no password.

Losing the store costs the password and the lock settings, nothing
else: the machine boots as a fresh one.

## Settings

The Security section of `settings` ([settings_app.md](settings_app.md))
sets, changes and removes the password, and edits the lock: at start,
minutes idle, and the serial console. Every change asks for the current
password first, in a dialog whose field shows `*` and is wiped when it
closes (`z_dialog_prompt_secret()`, `zdialog.h`). Afterwards it tells
`wm` to re-read the policy (`Z_WM_LOCK_RELOAD`).

## For apps

```c
#include "zauth.h"

z_auth_status_t st;
z_auth_status(&st);                         // is there a password? the policy
z_auth_check(pw, len, &wait_ms);            // Z_AUTH_OK, _E_BAD, _E_WAIT ...
z_wm_lock_request(Z_WM_LOCK_NOW);           // lock the screen
```

`z_auth_check()` blocks for about half a second. Say something on
screen before calling it.

## Cost

Kernel image: 8,984 bytes, of which SHA-256 is 1,696. To make room the
store's on-board test (`kv test`) is now left out by default; `make
KV_TEST=1` builds it back in for a hardware check. The kernel has
3,488 bytes of headroom now, with netserve's config keys in (704 with `kv test`).

## Open: encryption

The card is readable by anyone who holds it. Password-based encryption,
probably ChaCha20-Poly1305 with a key derived from the password, is
open, and to be discussed before anything is built. The questions it
opens:

- **Where:** a layer under FatFs encrypting sectors, an encrypted
  container file mounted as a volume, or files encrypted one by one.
- **Integrity:** Poly1305 needs 16 bytes of tag per unit. A sector has
  no room for one, which is why disk encryption usually uses a
  length-preserving mode without authentication; a file or container
  can carry tags.
- **The key:** any app can read any address, so a key in memory
  protects a stolen card, not a machine running hostile code.
- **Boot order:** the core apps can come from the card, and
  `/zeitlos.cfg` is read from it before anything could ask for a
  password.
- **Speed:** ChaCha20 in software against the card's throughput
  ([sdcard.md](sdcard.md)).
