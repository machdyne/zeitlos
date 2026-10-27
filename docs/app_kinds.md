# Kinds of app: desktop, screen, full-screen, dual

Every app with a picture of its own is one of four kinds. The kinds
were there before they had names; this page names them, so that what an
app owes the person using it -- its help, full screen, its boxes -- can
be said once for each kind.

| Kind | What it draws | Where it is seen | Its controls and boxes |
|---|---|---|---|
| **Desktop app** | a window at the desktop's resolution (640x480, one bit a pixel), often resizable | among the other windows; never in game mode | the desktop's: `zwin`, `zwidget`, `zdialog` |
| **Screen app** | one fixed **320x240 screen** | in a window, 1:1, in black and white -- or full screen (**F11**, game mode), doubled, in colour where the board has it; the same screen either way | its own, drawn on its screen |
| **Full-screen app** | a 320x240 screen | full screen only (game mode) | its own, drawn on its screen |
| **Dual app** | a desktop window **and** a 320x240 screen, in the same window | its desktop face among the other windows; its screen face in the window (in colour where the board has the colour window) or full screen | the desktop's in its desktop face, its own on its screen |

A screen app and a full-screen app are one kind of program -- a
320x240 screen, game mode, colour -- that differ only in whether they
also live in a window. The arcade runtime (`sw/common/games/zarcade.h`,
[arcade.md](arcade.md)) is made for them; [game_mode.md](game_mode.md)
is the hardware under them.

## What each kind does

**Desktop app.** F1 opens its help box ([app_help.md](app_help.md));
F11 on a resizable one is wm's full screen -- still the same window,
without its titlebar and the dock ([keys.md](keys.md), "Full screen
(F11)"). It has no game mode.

**Screen app.** F11 goes between the window and full screen. Full
screen there is no titlebar and no desktop, so what a window's title
would say -- the open file -- the app shows on its own screen, and the
window's title can be just its name. Esc, full screen, is the app's
own. Its help and its boxes are shown inside the app, on its screen,
by the screen toolkit ([screen_ui.md](screen_ui.md)). (An app that does
not link it yet still opens the desktop's help box in the window, and
nothing full screen.)

**A desktop app's figure in colour.** `learn` is a desktop app, but it
asks for the colour window like a dual app's screen face
(`Z_WIN_FLAG_COLOR | Z_WIN_FLAG_COLOUR_LATER`, then `Z_WM_COLOR_WANT`)
while a lesson's figure, at most 320x240, shows whole. The colour
window covers only the figure; the rest of the window stays the
desktop's ([learn_figures.md](learn_figures.md#colour)). It is the one
desktop window that shows colour of its own.

**Full-screen app.** As a screen app full screen, always: started from
the dock or a shell, it takes the screen and gives it back when it
ends.

**Dual app.** One window with two faces. Its **desktop face** is a
desktop app's: resizable, black and white, the desktop's widgets and
boxes. **F10** -- or the colour icon on its titlebar, a rainbow --
turns the same window into its **screen face**: 320x240, the app's
screen as a screen app draws it, in colour while the window has the
colour window ([color.md](color.md)); F10 or the icon again, or
resizing the window, go back, to the size it had (or the new one).
**F11** is full screen from either face: the screen face, in game mode.
On its screen its help and boxes are the screen toolkit's. The arcade
runtime takes the window and gives it back (`ar_attach()`,
`ar_detach()`, [arcade.md](arcade.md), "A dual app"). Colour is never
shown on the desktop face: the desktop, its icons and its windows are
one bit a pixel, and colour is only ever a program's 320x240 screen.

## The apps

| Kind | Apps |
|---|---|
| Desktop | ask, backup, bench, cal, calc, casino (the lobby), clock, colwin (a test of the colour window), convert, draw, files, gpu3d, gpudemo, hello_win, hex, info, irc, keyboard, learn, logic, mesh, midi, mmod, play, print, read, settings, sheet, slide, term, text, track, web |
| Screen | basic, blocks, bricks, kidgames, pix, snake, space3d |
| Full-screen | mausrun, planedemo |
| Dual | blackjack, chess, craps, poker, roulette, slots, view |
| Not settled | chip8: a window at 1x, 2x or 4x of its display, and game mode -- it stays as it is. |

Not counted: programs without a picture of their own -- the services
(`fs`, `net`, `usb`, `cron`, `tts`, ...) and the programs that run in a
terminal (`posix`, `vi`, `repl`, `zetta`, `zcc`, ...). `wm` is the
desktop itself.
