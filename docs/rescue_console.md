# Rescue console

The kernel shell on the monitor, typed at on the USB keyboard, with no
window manager in the way. For a machine whose desktop does not come
up -- a bad `wm`, a broken card, a display left blank -- when there is
no serial cable to hand, and for a quick look at the console any time.
The serial console keeps working alongside it: both show the same
prompt and the same output, and either can type.

**Ctrl+Alt+Esc, any time on the desktop.** wm hides every window and
the console takes the screen; nothing is stopped, the apps keep
running. **Ctrl+Alt+Esc again, or `exit`**, and the windows come back
where they were. Not from the lock screen (that would be a shell past
the lock); a lock asked for while it is up (`lock`) takes it down and
locks.

**Hold ESC on the USB keyboard while the machine boots, until the
console appears.** Init is skipped, as with ESC on the serial console;
`init` starts the desktop when you are ready.

It also comes up by itself:

- when `wm` cannot be started (not in flash or on the card, or no
  memory for it) -- and the rest of init, `net` included, still runs;
- when `wm` crashes or exits. `run wm` starts it again.

Starting `wm` by any route (`init`, `run wm`) hands the screen back.

**Ctrl+Alt+Del** from the console asks to reboot, as on the desktop,
and Cancel comes back here ([window_manager.md](window_manager.md#rebooting));
with no `wm` behind the console, it says to type `reboot`.

## Status (October 2026)

Tested on hardware: boot with ESC held, the shell, the takeover. The
8x8 font, Ctrl+Alt+Esc and `exit` are built and host-tested
(`test_rescue` 54 checks; wm's `test_lock` 45, the rescue part 16),
awaiting hardware.

## Using it

- 80 x 60 characters, the 8x8 font (each glyph fills its cell; the
  5x8 font's gaps were hard to read on hardware), white on black; an underline is the
  cursor. On composite video, which always shows 320x240 of the
  picture, 40 x 30 in that corner.
- On entering, it shows the end of the console log (about a screenful),
  so the boot messages or the crash report that brought it up are
  there.
- Keys: a US layout, Shift, Caps Lock and Ctrl (Ctrl+letter). Enter,
  Backspace, Tab, Delete. No arrow keys, function keys or keypad: the
  kernel shell has no use for them.
- Everything the shell can do works -- `ls`, `run`, `xf`, `tget`,
  `format`, `passwd` -- with one exception: `passwd reset` refuses keys
  that a *program* injected (the on-screen keyboard, `automate`), as it
  refuses `console0` input; keys typed on a real keyboard are accepted.
- A locked console asks for the password here as on serial.

## The boot window

The kernel waits 500ms before init (`starting init in 500ms -- press
ESC to cancel ...`). During it, it also looks at the USB keyboard's
key registers, which hold the keys that are down now -- so a held key
is simply there, with no events or key repeat involved, and a stray
press cannot bring the console up.

On boards with the USB host (Lakritz), the keyboard only reports once
`usb` has set it up, which may take longer than 500ms. So while a
device on a root port is still being reset, and for 400ms after it is
enabled, the window stays open -- unless a keyboard has already
appeared, and never past 2.5s. With nothing plugged in it closes on
time. When it stays open longer than 600ms the shell says so:

```
init: boot window open 820ms (a USB device was coming up)
```

Boards with the `USB_HID` controller set their keyboards up in
hardware, long before the window, so they never wait.

## Limits

- **Other graphical programs** still running after `wm` crashed may
  draw over the console; it repaints only what it writes.
- **`kill` on `wm`** does not bring it up: a deliberate kill is meant.
  `kill -c <pid>` (as a crash) does.
- **No display** (a bitstream without the GPU): nothing to show; the
  serial console is the console.
- **No keyboard** (no `usb`, or it crashed): the console still shows,
  and the serial console still types.

## How it works

All of it is `sw/os/rescue.c` (about 4KB of kernel; 5KB with the hooks); the rest of the
kernel calls it at a few points (`rescue.h`):

| where | call | what |
|---|---|---|
| `sh.c`, boot window | `rescue_boot_poll()` | ESC held? keep the window open for USB? |
| `sh.c`, boot and `init()` | `rescue_enter()` | ESC was held; `wm` could not be started |
| `kernel.c`, `k_proc_create_exec()` | `rescue_exec()` | `wm` starting: hand over |
| `kernel.c`, `k_proc_exit_record()` | `rescue_ended()` | `wm` crashed or exited: take over |
| `uart.c`, the console log | `rescue_note()` | a byte was logged |
| `uart.c`, `k_uart_putc()`, `k_uart_wait_rx()` | `rescue_pump()`, `rescue_pending()` | draw it |
| `hid.c`, `hid_push()` | `rescue_key()` | a key event: to the shell, not the apps |

- **Output** is the console log ([console.md](console.md)): every byte
  for the serial console is already recorded there, so the rescue
  console needs no output path of its own. It keeps its own position
  in the log and draws what is new.
- **Only pid 0 draws**, and never in an interrupt, nor while it holds
  the timer interrupt off itself (it draws that output later). pid 0
  draws as it prints; output from other processes or from an interrupt
  (a crash report) wakes it if it is waiting for input
  (`rescue_note()`). It draws one 128-byte piece of the log at a time
  and looks at the keyboard in between, so a process printing without
  pause cannot keep the shell from reading what is typed.
- **wm is known by its pid**, recorded when it is created, as well as by
  its registered name: a broken wm can crash before it registers.
- **The screen**: glyphs are CPU byte stores into VRAM (an 8x8 glyph
  is one byte a row); scrolling
  and clearing are blitter copies and fills, programmed with interrupts
  masked from the idle check to the start, as `zgfx.c` does, so a
  program still drawing after wm died cannot interleave its own
  settings. A blitter that never goes idle stops the drawing (the
  serial console carries on). Game mode is switched off
  on entering. Of the VT100 sequences, cursor movement, home and erase
  line/screen are understood -- what the shell's own line editing uses
  -- and the rest are dropped.
- **Keys** come through `hid_push()`, the kernel's single key queue:
  while the rescue console is on, it takes each press, turns it into a
  character and puts it into the serial receive ring
  (`k_uart_rx_put()`), so the shell reads it as typed on the serial
  console. Nothing is queued for apps meanwhile, so `wm` starts with no
  stale keys.

## Ctrl+Alt+Esc

It is wm's screen lock with the kernel's console where the lock screen
would be (`wm.c`, `rescue_engage()` and `rescue_release()`, next to
`lock_engage()` and `lock_release()`):

1. The hotkey is wm's, by key (ESC is the same key on every layout),
   acted on at a safe point in its loop -- after a drag, as the lock.
2. An app holding the screen is revoked and game mode's viewport put
   away; every window is frozen (bounded wait for the acks) and given
   an empty region, so nothing an app draws reaches the glass. wm draws
   nothing either while it is up (`WM_HIDDEN()`).
3. `Z_SYS_RESCUE` 1: the kernel shows the console. While it is up the
   kernel turns every key press into shell input, except Ctrl+Alt+Esc,
   and passes the releases on, so wm does not think a key held as it
   hid is still down.
4. Back -- Ctrl+Alt+Esc, or `exit` (the shell sends wm `Z_WM_RESCUE`):
   `Z_SYS_RESCUE` 0, then every window its region and a repair of the
   whole screen, as `lock_release()` does. The apps redraw their
   windows.

If wm crashes while it is up, the console simply stays (that is what
it does for a crash anyway); `exit` then says there is no desktop and
`run wm` starts one. The cost: ~0.7KB of kernel, ~0.8KB of wm.

## Testing

```
cc -std=gnu99 -Wall -I sw/common -o /tmp/test_rescue \
   sw/os/tests/test_rescue.c sw/common/zfont_data.c && /tmp/test_rescue
```

The terminal (where characters land, wrapping, scrolling, the VT100
subset, the cursor) on an array in place of VRAM, and the key table.

On hardware: hold ESC from power-on; type `ls` and `help`; `init`
(the desktop appears); `kill -c` on wm's pid (`ps`) for the takeover,
then `run wm`; and boot with no keyboard, and with only the ethernet
adapter, to see the window close on time.
