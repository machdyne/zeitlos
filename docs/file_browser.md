# files

A file browser. `sw/apps/files`. A core app: it is in flash and works
with no card ([flash_apps.md](flash_apps.md)).

It shows the card, `/ram` and `/usb` when they are there, and what is
in flash: the core apps in `/apps` and the files in flash (`/docs/welcome.txt`),
read-only, merged into the same directories ([flash_apps.md](flash_apps.md),
"Files in flash"). With no card, `/` holds `apps`, `docs` and whatever
of `/ram` and `/usb` is mounted.

```
> run wm
> run files
```

Most of this app is the `z_flist_t` widget (`sw/common/zflist.h`),
which was factored out of the file dialogs on exactly the assumption
that a browser would want the same thing. What's left is the window
around it, eight buttons, and the decision about what double-clicking
something means.

## Using it

**F1** shows a short help box; its **Read docs** button opens this page ([app_help.md](app_help.md)).

Double-click a folder to enter it, or the `..` row to go up. Arrow
keys, PageUp/PageDown, Home/End move the selection; Enter opens;
Backspace goes up a level. The mouse wheel scrolls the list three rows
a notch without moving the selection, here and in every app's Open and
Save dialogs ([user_input.md](user_input.md#scroll-wheel)).

**Tab** moves between the list and the eight buttons, **Shift+Tab**
goes back, and **Enter** or **Space** presses the focused button. The
focused control draws a ring around it. This app is fully usable with
no mouse at all, which is a first-class case in this system — arrows
deliberately don't move between buttons, because they belong to the
list and Tab is the only way across.

| button | key | does |
| --- | --- | --- |
| Open | Enter | opens the selection — same as a double-click |
| New Dir | | prompts for a name and creates it in the current directory |
| Rename | F2 | prompts with the current name; the new one stays in this directory |
| Copy | | prompts "Copy to:" with the file's own path, to edit |
| Move | | prompts "Move to:" the same way |
| Delete | Delete | asks first, naming the file, then removes it -- a folder with everything in it |
| Convert | | prompts for a name whose ending picks the format, and converts ([convert.md](convert.md)) |
| Find | F3, Ctrl+F | finds files by name or by text inside, in this folder and below; **Stop** while a find, copy or delete runs |
| | Esc | stops a find, copy or delete at once; with results up, back to the folder |
| | F5 | reads the folder again |
| | F12 | home: back to the folder `files` started in (the one it was launched with, or `/`) |

Each row shows the entry's **size and date** on the right: bytes up to
9999, then `K` and `M` to one decimal place, and `<dir>` for a folder.
Dates are UTC, the RTC's time ([filesystem.md](filesystem.md#timestamps)).
The column is only drawn when the row is wide enough to keep twelve
characters of the name in front of it, so a narrow window shows names
alone rather than truncating them to nothing.

### Rename, copy and move

**Copy** and **Move** start the prompt from the file's own full path,
because the common cases are then an edit at the end of the line: a
new name, or another directory in front of the same name. A destination
that is an **existing folder** means "into it", as `mv` does in the
posix shell. Copying onto an existing file asks before replacing it;
moving onto one is refused ("destination exists").

**Move within a volume** is a single rename and instant, whatever the
size. **Between volumes** (the card and `/ram` or `/usb`) a rename is
impossible, so it is a copy followed by a delete, and the original is
removed only once the copy has succeeded.

**Folders** copy, move and delete with everything in them. A copy (of
a file or a folder), a move between volumes, and deleting a folder run
a step at a time ([zwalk.md](zwalk.md)) while the window stays usable:
the line above the list counts files and bytes, the list can still be
browsed, and **Esc** or **Stop** ends it before the next step. Stopped
part-way:

- a **copy** keeps what it had finished and says so -- never half a
  file: the file it was writing is deleted;
- a **move** stopped while copying leaves the original untouched; once
  the copy is complete the original is deleted, and stopping that says
  part of it is still there;
- a **delete** says part of it is gone already.

A folder cannot be copied into itself ("the destination is inside the
folder"), and a copy never overwrites a folder.

A file some program has **open for writing** cannot be renamed or
moved ("in use"). Renaming it would lose what that program then writes;
[filesystem.md](filesystem.md#what-a-rename-refuses-and-why) says why.

The file just renamed or copied stays selected after the list reloads.

Buttons are laid out from the **left**, deliberately: right-aligning
them the way a dialog does would put the last one underneath the resize
grip.

Deleting a file uses `fs_unlink()` at once; a file in use cannot be
deleted, and the message says so.

### Find

**Ctrl+F** (or **Find**) asks what to look for, in the folder shown and
every folder below it:

| typed | finds |
| --- | --- |
| `report` | names containing "report", any case: `Report.md`, `old-report.txt`, a folder `reports` |
| `*.md` | names matching the pattern: `*` any run of characters, `?` one |
| `"invoice"` | files with "invoice" inside them, any case (ASCII) |
| `*.txt "invoice"` | both: `.txt` files with "invoice" inside |

The results replace the folder while they are up, as paths below
where the find started, and arrive as they are found: the line above
says how many, and how many files have been looked at. **Esc** stops
the find at once and keeps what it found; Esc again goes back to the
folder. **Enter** or a double-click opens a result as the list would
(a folder is gone into); **Backspace** goes to the result's folder with
it selected -- to rename, copy or delete it there. **Ctrl+F** again
starts a new find.

How fast: a name find costs one directory listing per folder; a text
find reads every file, 2KB a step, so it goes at the card's speed --
the window answers throughout either way. A find keeps over a thousand
results in its arena (64KB, only while a find, copy or delete runs or
results are up), about eighty without one (a board with too little
memory); the line says "(not all)" when results or a
very large folder were cut short. Folders more than 16 deep are not
entered.

### Convert

**Convert** turns the selected file into another format with
`convert` ([convert.md](convert.md)). The prompt starts from the same
path with the ending the file most likely wants -- the other system's
format for this system's own files (`.txt`/`.md` to `.docx`, `.zss`/
`.csv` to `.xlsx`) and this system's for the other's (`.docx`, `.odt`,
`.rtf`, `.epub`, `.html` to `.md`; `.xlsx`, `.ods`, `.tsv` to `.zss`) --
and an archive's own name without its ending, as the folder to extract
into. Change the ending to pick another format: `report.odt`,
`report.rtf`, `budget.ods`.

`convert` runs as its own process with a small window: a progress bar
and **Cancel**, which stops it at once and leaves nothing behind. The
browser does not wait for it -- it stays usable meanwhile -- and
re-reads the folder, selecting the new file, when it is done. One
conversion at a time from here.

## Opening a file

By extension, through the shared table in `sw/common/ztype.h`:

| extension | app |
| --- | --- |
| `TXT`, `ASC`, `CFG` | `text` |
| `MD` | `read` |
| `ZBM` | `draw` |
| `ZSS`, `CSV` | `sheet` |
| `ZSL` | `slide` |
| `BMP`, `GIF`, `JPG`, `JPEG`, `PNG`, `PNM`, `PBM`, `PGM`, `PPM`, `SVG` | `view` |
| `WAV`, `AU` | `play` |
| `MOD` | `track` |
| `MID`, `MIDI` | `midi` |
| `STL` | `gpu3d` |
| `HTM`, `HTML` | `web`, as a local page ([web_app.md](web_app.md#local-files)) |
| `BIN`, `ROM`, `IMG`, `DAT` | `hex` |
| `CH8`, `SC8`, `XO8` | `chip8` |
| `NET` | `bench` |
| `ZDS` | `automate` |
| `DOCX`, `ODT`, `RTF`, `EPUB` | `convert`: as Markdown, in `text` -- or `read` when larger than `text` can take on this machine |
| `XLSX`, `ODS`, `TSV` | `convert`: as a `.zss`, in `sheet` |
| `ZIP`, `TAR`, `TGZ`, `GZ` | `convert`: extracted beside the archive, shown here |
| *(none)* | run it, if it's really a program |
| anything else | "no application is associated" |

Opening another system's document or table converts it into `/tmp`
(the RAM disk: gone at the next boot), so the original is never
touched; the first **Save** of the converted copy asks where to keep
it, starting in `/home`. An archive is extracted beside itself, into a
folder of its name (`photos-2` if `photos` is taken), which then opens
in a new browser window.

An extension nothing claims gets a dialog rather than silence — a
double-click that appears to do nothing reads as the app being broken.

**A file with no extension is only assumed to be a program, and the
assumption is checked.** `z_ftype_is_executable()` reads the first four
bytes and requires the `ZEXE` magic. That check is not optional: the
loader treats a file *without* the magic as the legacy raw executable
format and will load and jump into it regardless (see `zexec.h`,
"Backward compatibility"), so without it double-clicking a `README`
would execute it.

The filename reaches the launched app through wm's pending launch
argument — see "Launch arguments" in `docs/widgets.md`. The argument is
set *before* `z_proc_run()`, because the new process can reach its own
startup before the browser runs again.

`Z_PROC_RUN_NAME_MAX` was raised from 32 to 64 for this. The browser
can hand the loader a path several directories deep, and a longer name
is silently truncated, which surfaces as a confusing "no such file"
rather than an error about length. 64 matches `Z_FLIST_PATH_MAX`, which
is what the browser can produce.

`SVG`, the sound files, `MOD`, `MID`, `STL` and `HTML` are new in 0.0.7:
every one of those apps already took a file as its launch argument, and
the table simply did not name them, so a double-click said "no
application is associated" for a file the system could open. 
`sw/common/tests/test_ztype.c` now checks the table against the tree --
every app it names is a program `sw/apps` builds, and no extension is
listed twice:

```
cc -std=gnu99 -Wall -I sw/common -o /tmp/test_ztype \
   sw/common/tests/test_ztype.c sw/common/ztype.c && /tmp/test_ztype
```

## Adding a file type

One line in `z_ftypes[]` (`sw/common/ztype.c`):

```c
{ "WAV", "player", "Sound" },
```

`app` is what `z_proc_run()` expects — a bare program name, no path and
no extension. Nothing else needs changing; the browser, and anything
else that later asks "what opens this?", read the same table.

## Known limits

- **Each open starts a fresh process.** Double-clicking two `.TXT`
  files gives two independent `text` instances, each roughly 50KB of
  image plus 16KB of stack and heap. On a 1MB board that adds up.
  Handing the file to an already-running instance would need a way to
  find one and ask it, which the pid registry could support but nothing
  does yet.
- **The listing is bounded** at `Z_FLIST_MAX` (256) entries, or fewer
  when the names are long (the list's 8KB name pool). It was 128,
  which `/docs` on the card -- 150 files -- outgrew. A fuller
  directory is listed in part, and the path line above the list says
  "(partial list)", so a file that is on the card but not shown has an
  explanation. `ls` in the posix shell has the same bound and says
  "... (truncated)".
- **Folders copy as walks.** A folder copies, moves
  and deletes with everything in it, as a walk (`zwalk.h`); within
  a volume a move is one `FS_RENAME`. A folder is not copied over an
  existing folder of the same name.

## Mounting

There is no card-detect line on this hardware (SPI only), so nothing
notices a card being inserted or swapped — the volume is mounted once
at boot. `mount` at the shell is the manual override, and also recovers
a card that failed to come up at boot because it was slow or inserted
late.

It **refuses while any file handle is open**. Remounting out from under
an open `FIL` leaves that handle describing cluster chains from the
previous mount, and the next write through it corrupts the card —
worse than making you close something first. `k_fs_open_count()`
(`sw/os/fsapi.h`) is what it checks.
