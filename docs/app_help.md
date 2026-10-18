# Help in apps: F1

**F1** in an app opens its manual: the app's own page from `docs/`, in
`read`, at the heading **Using it**.

```
F1 in sheet  ->  read /docs/sheet_app.md#using-it
```

Apps with it in 0.0.7: `text`, `files`, `read`, `sheet`, `hex`.

## Why the docs, and not a help screen

There is one description of each app, and it is the one in `docs/`:
the release copies `docs/*.md` to the card's `/docs` as they are
([layout.md](layout.md)), `read` renders them with working links, and
they are what GitHub shows. A help screen inside each app would be a
second copy of the same facts, in a second place, compiled into a
binary that has a size budget -- and it would drift from the first the
first time either changed. This way the help is whatever the docs say,
and costs an app about forty bytes.

**F1** rather than Ctrl+H: Ctrl+H is Replace in `text` (as in most
editors), and F1 is help nearly everywhere else -- including in this
system's games, which already show their rules on it.

## What an app's page needs

A section headed exactly `## Using it`, **first** after the
introduction, written for someone using the app rather than changing
it: what it is for, the keys, the commands, the limits. Everything
about how the app works -- design, tests, history -- comes after it,
for whoever reads on. A page without the heading still opens, at the
top.

## Adding it to an app

```c
#include "../../common/zdialog.h"

if (keysym == Z_KEY_F1) { z_dialog_help(&dlg_ctx, "sheet_app"); return; }
```

`z_help_open(doc)` (`sw/common/zdialog.c`) checks that
`/docs/<doc>.md` is there, sets the launch argument
`/docs/<doc>.md#using-it` and runs `read`. `z_dialog_help()` is the
same plus a dialog when there is nothing to show. Neither uses printf,
so they cost nothing an app does not already have.

## Without read, without a card

`read` is not a core app, and `/docs` is on the SD card, so a system
running from flash alone may have neither:

| | F1 does |
|---|---|
| card with `read` | opens the page in `read`, at "Using it" |
| card without `read` | opens the page in `text` (a core app, always in flash), as plain Markdown from the top |
| no card (flash only) | says, in a dialog, that the manuals are in `/docs` on the SD card |

The flash has no room for the manuals: its files reserve (48KB) is
already the welcome page and the Scheme reference. One alternative was
considered and left: a short built-in help screen per app, which would
be the second copy of each page this design exists to avoid. `read` takes a `#heading`
on its launch argument the way it takes one on a link: the heading's
GitHub-style slug, found by a pass over the document. (An all-digit
fragment is a byte offset, as `ask` uses.)

Board checks for the final phase: F1 in each of the five apps opens
the right page at the right place; with `read` removed from the card it
opens in `text`; with no card it shows the dialog.
