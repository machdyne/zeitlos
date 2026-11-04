# launcher

Kind: a **screen app** ([app_kinds.md](app_kinds.md)) -- the console
profile's front end ([profiles.md](profiles.md)).

On a console bitstream wm starts it in place of the desktop, and starts
it again if it stops. It is a menu of what is in `/apps`, in game mode,
on the arcade runtime ([arcade.md](arcade.md)): the games and demos
first (mausrun3d, mausrun, space3d, snake, blocks, bricks, chess, ...),
then everything else, the system's own programs left out.

| Key | Does |
|---|---|
| Up, Down (or a gamepad) | choose; the list scrolls |
| Left, Right | a page at a time |
| Space, Enter (the pad's A, START) | start it |

Starting one gives it the screen (the launcher leaves game mode, and the
program takes it); when it ends -- Escape, on a console -- the menu is
back, `/apps` read again.

On a desktop it is an ordinary arcade app (`run launcher`), which is how
it is tried without a console bitstream.
