# Function keys

The function keys mean the same thing in every app.

| Key | Means | |
|---|---|---|
| **F1** | **Help** | the app's help box ([app_help.md](app_help.md)), in a window and in a full-screen window; **in game mode, nothing** |
| **F2** | **Rename / edit in place** | `files`: rename; `sheet`: edit the cell |
| **F3** | **Search / next** | no search yet: ask what to find (as Ctrl+F); then the next match -- `text`, `sheet`, `web`, `files` |
| **F4** | **The app's prompt** | gives the command or input box the focus -- `sheet`'s command line (and its `/` menu), `web`'s address field; in apps whose prompt always has the focus (`chess`, the casino tables, `basic`, `ask`, `mesh`, `irc`) typing goes there anyway |
| **F5-F8** | **The app's own** | listed in its F1 box -- `web` F5 reload, `files` F5 read the folder again, `sheet` F5 chart, `bench` F5/F6, `midi` F5-F7, `mesh` F5/F6, `chip8` F5-F8 |
| **F9, F10** | **Reserved** | bound by nothing, for later |
| **F11** | **Full screen** | a resizable window: full screen -- the whole screen, no titlebar, no dock (wm does it); an app with a game mode: game mode |
| **F12** | **App home / reserved** | back to where the app starts -- `term` (disconnect, the welcome panel), `web` (the page it started with, or the bookmarks), `files` (the folder it started in); elsewhere reserved |

A key with a meaning is that meaning or nothing: an app that has no
use for it leaves it unbound rather than giving it another job.

In code the meanings have names (`sw/common/zkbd.h`): `Z_KEY_HELP`,
`Z_KEY_RENAME`, `Z_KEY_FIND`, `Z_KEY_PROMPT`, `Z_KEY_FULL_SCREEN`,
`Z_KEY_APP_HOME`. `tools/check_fkeys.py` runs with every apps build and
fails it for a meaning key bound by number, anything on F9 or F10,
F12 outside the apps with a home, or a help text that puts full screen
on another key.

## term

`term` is the exception: it sends F1-F10 to the program running in it
(as the usual VT sequences) instead of interpreting them, connected or
not. Its welcome panel shows what there is to know, so it has no help
box. F11 is wm's (full screen, as for any window) and F12 is term's
home: disconnect, back to the welcome panel -- from anywhere, whatever
the far end is doing. Somewhere new is typed on the welcome panel (or
its OPEN button), so F12 and then typing is how you go elsewhere.

## Full screen (F11)

**Windows.** F11 on a resizable window is wm's: the window takes the
whole screen (or as much of it as the app's size limit allows, centred),
its titlebar goes and the dock hides. **F11 or Esc** brings it back --
Esc so that nobody who pressed F11 by accident is stuck without knowing
the key. While a window is full screen Esc is wm's; the app gets it
again once the window is back. Help boxes and other dialogs open over a
full-screen window as over any other.

The app is told through the usual rect messages: the top bit of their
`h` says there is no titlebar (`Z_WM_RECT_BARE`), `z_win_parse_rect()`
keeps it in `z_win_t.bare`, and `z_win_content_rect()` (zwin.c) then
starts the content at the top edge. Apps need no code for it.

**Game mode.** An app with a game mode -- `space3d`, `chess`, the casino
tables, `kidgames`, `basic`, `chip8`, `snake`, `blocks`, `bricks` --
creates its window with `Z_WIN_FLAG_GAME_MODE`, and F11 goes to it: game
mode, and F11 again back to the window. Esc leaves game mode too, armed
on release; where a game has screens inside it, Esc goes up one level at
a time (`kidgames`: a game, then the menu). `basic` keeps Esc for BASIC
(it stops a program), so F11 is its way out. In game mode F1 does
nothing. Apps keep the mode they start in.

`gamedemo` is always in game mode and leaves with Esc.

## The prompt (F4) and the `/` menu

Apps here have no drop-down menus. Many have a command or input line
instead, and pressing `/` at the start of it opens a selectable menu of
its commands (`sheet`; new apps follow). F4 gives that line the focus
from anywhere in the app.

## What changed (0.0.7)

| App | Before | Now |
|---|---|---|
| `space3d`, `chess`, `blackjack`, `craps`, `poker`, `roulette`, `slots`, `kidgames`, `snake`, `blocks`, `bricks` | full screen on F2 | F11 |
| `basic`, `chip8` | full screen on F6 | F11 |
| every resizable window | Alt+= maximize only | F11 full screen (Alt+= still maximizes) |
| `chess`, the casino tables | F1 in game mode: a rolling help line | nothing; the tables' F1 in a window is now a help box |
| `chip8` | F1-F3 scale, F4 profile, F6 game mode, F7/F8 speed, F9 screenshot, F10 debugger, F11 step, F12 run/pause | F1 help, F5 reset (Shift: profile), F6 scale (Shift: greys), F7 debugger (Shift: screenshot), F8 run/pause (Shift: step), - = speed, F11 game mode |
| `term` | F1 help box on the panel, F11 the Open bar | F1-F10 to the program; F11 full screen; Open bar by typing on the panel |
| `midi` | F2 drums, F3 mono, F4 panic | F5, F6, F7 |
| `mesh` | F2 console log, F4 notifications | F5, F6 |
| `sheet` | F10 chart | F5; F4 the command line |
| `files` | -- | F3 find, F5 read again, F12 home |
| `web` | -- | F4 address, F5 reload, F12 home |
| `info` | no help | F1 help box |
