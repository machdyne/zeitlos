# convert

Kind: a **desktop app** ([app_kinds.md](app_kinds.md)).

`convert` turns documents, tables and archives from one format into
another: a Word file into something `text` can edit, a sheet into an
`.xlsx` someone else can open, a `.tar.gz` into a folder, a folder into
a `.zip`. It is a program of its own -- with [a window](#the-convert-window)
when it is started by itself, from the dock's Work group -- run from
the shell or by an app, and the formats themselves are a library,
`sw/common/formats/`, that nothing else links.

```
convert                                  the window
convert report.docx                      open it (as files does)
convert report.docx /tmp/report.md       a document, by the names
convert -t odt notes.txt /home/notes.odt the output type named
convert budget.zss /home/budget.xlsx     a table
convert photos.zip /home/photos          an archive, into a folder
convert /home/photos /home/photos.zip    a folder, packed (.zip .tar .tar.gz)
convert /home/photos                     ... into photos.zip beside it
convert log.txt.gz /tmp/log.txt          one gzipped file
convert -l                               the formats
```

The output is written as `OUT.part` and renamed to `OUT` only once it
is complete: `OUT` is never half a file, and a conversion that fails
or is stopped leaves nothing behind but what was there before.

## Formats

| Kind | Read | Written |
|---|---|---|
| Documents | `txt` `md` `html` `rtf` `docx` `odt` `epub` | `txt` `md` `html` `rtf` `docx` `odt` |
| Tables | `csv` `tsv` `zss` `xlsx` `ods` | the same |
| Archives | `zip` `tar` `tar.gz`/`tgz` (to a folder), `gz` (to a file) | `zip` `tar` `tar.gz`/`tgz`, from a folder ([Packing a folder](#packing-a-folder)) |

A document becomes another document and a table another table; a
document cannot become a table. The input's type comes from its name
and is then checked against its first bytes, so a `.docx` renamed
`.zip` is still read as a document, and a zip is told apart from the
formats built on one (docx, xlsx, odt, ods, epub) by what is inside it.

## The convert window

`convert` with nothing to do -- its icon in the dock (Work, or More
apps on the wide row), or `run convert` -- opens a window:

| | |
|---|---|
| **From** | the file or folder, by **Choose...** (Ctrl+O): Choose on a folder takes the folder, Enter or a double-click goes into it |
| **To** | what it can become, by **<** and **>** (or Left and Right): a document any document format, a table any table format, a folder `.zip`, `.tar` or `.tar.gz`, an archive a folder of its files |
| **Convert** | asks where the result goes (a Save dialog, starting beside the original, with its name and the new ending), then does it, the progress bar in the window; **Cancel** (or Esc) stops it and leaves nothing |
| **Open** | the result: a document in `text` or `read`, a sheet in `sheet`, a folder or a new archive in `files` |

The ending typed into the Save dialog wins over the choice in **To**,
as in `files`' Convert. Tab moves between the buttons, Enter presses
the one with the focus, F1 is help, Esc closes the window.

## Packing a folder

A folder becomes a zip, a tar or a tar.gz, its own name the first part
of every path inside (`photos/2024/a.jpg`), as `zip -r` and `tar` make
them -- so anything unpacks it, and unpacking it here makes a folder
holding `photos` ([Opening a file](#opening-a-file)). From `files`,
**Convert** on a selected folder offers `<name>.zip` beside it; change
the ending for `.tar` or `.tar.gz`.

- **zip** deflates each file (stores it if it is a kind that is
  compressed already: jpg, png, mp3, zip, docx ...); **tar.gz** deflates
  the whole tar. The compressor is small: text comes out at about half
  its size, a quarter to a third bigger than `gzip -9` would make it.
- **Dates** are the files' own; **names** are UTF-8 (zip says so);
  folders are entries of their own, so empty ones come back.
- An archive being written inside the folder it packs is not packed
  into itself.
- **Limits:** 16 folders deep (deeper ones are left out, and convert
  says how many), 65,535 entries in a zip, 256MB in all (`ZF_OUT_MAX`).
  A zip's list of entries is kept in memory until the end, about 30
  bytes and the name each.

### What a document keeps

Paragraphs; headings 1-6; list items, nested; preformatted (code)
blocks; **bold** and *italic*; line breaks and tabs; every Unicode
character, including those outside the Basic Multilingual Plane.

That is what this system's editors hold, and so what is worth carrying
across. Left out: images, tables (their cells' text comes through, one
paragraph per cell, or tab-separated in RTF and HTML), footnotes,
comments, headers and footers, tracked deletions, page layout, fonts
and colours.

Per format:

- **docx** -- headings by style (`Heading 1`..., `Title`) and by
  outline level, including styles based on one; list items by numbering
  properties or a list style (`List Bullet 2` is level 2); preformatted
  by a code or plain-text style. Written with a styles part (Title,
  Heading1-6, a Courier "Plain Text"), one bullet list of nine levels,
  and stored (uncompressed) zip entries, which Word, LibreOffice and
  every docx reader accept.
- **odt** -- `text:h`, nested `text:list`, spans whose automatic styles
  are bold or italic, `text:s`/`text:tab`/`text:line-break`. Written
  with the `mimetype` entry first and stored, as the standard requires.
- **rtf** -- what WordPad, TextEdit and Word write: `\par`, `\line`,
  `\tab`, `\b`, `\i`, `\outlinelevelN` or a stylesheet entry named
  "heading N", `\ls`/`\ilvl` or a paragraph that begins with
  `\bullet`, a fixed-pitch font as preformatted; Windows-1252 bytes,
  `\'hh`, `\uN` with its fallback skipped, surrogate pairs; a field's
  result without its instruction. Written as RTF 1.x with `\uN` for
  everything past ASCII.
- **html** -- read forgivingly: unclosed paragraphs end at the next
  block, whitespace collapses as a browser collapses it, `script`,
  `style`, `head` (except `title`), `nav` and the like are skipped,
  bytes that are not UTF-8 are read as Windows-1252. Written as one
  UTF-8 page.
- **epub** -- the spine, in reading order, each chapter through the
  HTML reader into one document; the book's `dc:title`. Books with DRM
  are refused.
- **md** -- the CommonMark people write: ATX and setext headings,
  `-` `*` `+` and numbered lists nested by indentation, fenced and
  indented code, `>` quotes, emphasis with `*` and `_`, code spans,
  links and images as their text, two trailing spaces as a break.
- **txt** -- one paragraph per line, blank lines kept. UTF-8, UTF-16
  (with its byte-order mark) or Windows-1252, told apart per character:
  a byte that is not part of valid UTF-8 is taken as Windows-1252, so
  an old Windows file reads correctly and a UTF-8 one is never misread.

### What a table keeps

Text, numbers and formulas, in their cells. The first sheet of a
workbook is read.

A formula crosses between formats only when `sheet` could evaluate it:
`+ - * / ^` and comparisons, parentheses, numbers, references
`A1`..`Z999` and ranges, `TRUE`/`FALSE`, and the sheet's functions --
`SUM AVERAGE AVG MIN MAX COUNT ABS INT ROUND MOD SQRT POWER IF AND OR
NOT PMT PV FV NPV VLOOKUP INDEX` ([sheet_app.md](sheet_app.md#formulas)). Anything else --
another function, another sheet, text operators, comparisons --
arrives as the value the other program last calculated, which xlsx and
ods both store with the formula. Going out, `AVG` is written `AVERAGE`
and ods gets its own reference syntax (`of:=SUM([.A1:.A3])`); xlsx and
ods made from a `.zss` carry the values `sheet` calculates, so a
program that does not recalculate on opening still shows them.

`zss` is `sheet`'s own format, and its limits are the sheet's: columns
A-Z, rows 1-999, 95 bytes a cell. Cells outside are left out and
counted (`convert` says how many); longer text is cut at a character
boundary. Text that would read as a number or a formula is written with
the leading `'` that keeps it text, and numbers in exponent form
(`1.5E-07`) are written out in full. Dates and times arrive as the
numbers or ISO text the other program stored.

## Opening a file

Given a file alone -- a double-click in `files`, which maps docx, odt,
rtf, epub, xlsx, ods, tsv, zip, tar and gz to it (`sw/common/ztype.c`),
or the Open dialog in `text`, `read` or `sheet` -- `convert` converts it
and opens the result:

| Input | Becomes | Opens in |
|---|---|---|
| a document | `/tmp/<name>.md` | `text` when it fits what `text` can take on this machine (its own 32KB, or an arena it can get with room to spare); otherwise `read` |
| a table | `/tmp/<name>.zss` | `sheet` |
| zip, tar, tar.gz | the folder `<name>` beside it (`<name>-2`... if taken) | `files` |
| gz | `<name>` beside it, without `.gz` | whatever opens that |

`/tmp` is the RAM disk -- gone at the next boot, and never the card at
an unexpected moment -- so the original is never touched, and `text`
and `sheet` send the first Save of a file in `/tmp` to Save As,
starting in `/home`.

### The window

A person is waiting, so `convert` shows a small window: the file's
name, a progress bar, and **Cancel** (Esc, Enter, Space or the close
icon do the same). It appears only after 300ms, so a small file never
flashes one. The conversion services it at its regular polls -- every
4KB read, at most every 20ms -- so it redraws and answers wm while
working, and Cancel stops the work at the next poll. A failure is said
in the same window, which stays until it is dismissed.

### Exporting from the apps

`files`' **Convert** button ([file_browser.md](file_browser.md#convert))
prompts for a name whose ending picks the format -- for a folder, the
archive to pack it into. `text`'s and
`sheet`'s **Save As** onto another system's format (`.docx`, `.odt`,
`.rtf`, `.html`; `.xlsx`, `.ods`, `.tsv`) writes the document to `/tmp`
and converts it from there. All three use the window above and do not
wait for it.

## Running it from an app

`sw/common/zconvert.h` is the caller's side:

```c
z_conv_t conv;
if (!z_conv_start(&conv, "/home/a.docx", "/tmp/a.md", NULL, 0))
	show_error(z_conv_strerror(conv.result));
...
// in the message loop
if (z_conv_msg(&conv, &msg)) { draw_progress(conv.permille); if (!conv.running) done(conv.result); }
// on a timer tick
z_conv_poll(&conv);
// the Cancel button
z_conv_cancel(&conv);
```

`z_conv_start()` writes a small job file to `/tmp` (the paths would
not fit the 96-byte launch argument), sets the argument `-j <job>` and
runs `convert`. Its flags: `Z_CONV_UI` gives the conversion the
window above, so the caller needs no progress display of its own;
`Z_CONV_OPEN` also opens the result. It uses no printf, so an app that
links it does not pay for one (`files` does not otherwise have it).
`text` and `sheet`, whose budgets are tighter still, write the job
file themselves (three lines: `in=`, `out=`, `ui=1`). `convert` sends `Z_CONVERT_PROGRESS` (tag: 0-1000 of
the input read, at most ten a second) and, at the end,
`Z_CONVERT_DONE` (tag: 0 or a negative `ZF_E_*`), then exits.

### Stopping it

**Cancelling is immediate.** `z_conv_cancel()` kills `convert` and
deletes `OUT.part`; nothing waits for `convert` to notice. That is safe
because the fs driver closes a dead process's open files before it
serves anything else ([fs_driver.md](fs_driver.md#owners-that-are-gone)),
so the delete finds the file closed, and because the output was only
ever `OUT.part`.

The other way round, `convert` checks a few times a second that the
process that started it is still the same process -- pid and serial,
since pids are reused -- and stops, cleaning up, if it is not. A
conversion nobody is waiting for is not left running.

A `convert` that crashes on a hostile file takes only itself down; the
caller sees `z_conv_poll()` report `Z_CONV_E_GONE` ("the converter
stopped").

## Untrusted input

Every file `convert` reads may have been made to hurt it, and the
library is written that way:

- lengths and offsets in a file are checked against what was read,
  never trusted; every structure has a bound (XML names and attributes,
  RTF group depth, styles, list levels, cell text);
- an archive entry is cut off at 64MB of output and a conversion at
  256MB (`ZF_ENTRY_MAX`, `ZF_OUT_MAX`), so a small file that inflates
  to gigabytes is an error ("too large"), not a full card;
- zip CRC-32s and sizes, gzip trailers and tar header checksums are
  checked; encrypted zip entries, ZIP64 and DRM'd books are refused;
- a path in an archive that is absolute, has a drive letter, or climbs
  out of the target folder with `..` refuses the whole archive before
  anything is written; links and devices in a tar are not made.

The host tests run every reader over truncated and corrupted copies of
every fixture, hundreds of cases, under AddressSanitizer and UBSan.

## Memory and speed

Nothing holds a whole file. Readers pull from a 4KB buffered stream and
push events to a writer; writers push to an 8KB write buffer. A zip is
read through its central directory, one entry at a time, through the
one 32KB inflate window; a zip is written with each local header
patched by seeking back once its entry is done, so nothing is held for
that either.

`convert` is about 158KB of code and 260KB of `.bss`, with a 64KB stack:
the largest pieces are the inflate window (32KB) and `sheet`'s own
document (107KB since 0.0.7's larger sheets, for `.zss`: the sheet's own
loader and evaluator are what make a `.zss` read here exactly as `sheet` reads it). xlsx's shared strings and an epub's
manifest need a table whose size the file decides; that goes in the
arena (a megabyte when the machine has one to give,
[kernel.md](kernel.md#arenas)), or, on a machine with none, an 8KB
buffer -- enough for small workbooks, and a clear "out of memory"
otherwise.

The conversion polls for cancelling and reports progress at every 4KB
read, so neither ever waits on a large piece of work.

## Source

| File | |
|---|---|
| `sw/common/formats/zf.h` | the API: I/O, events, readers, writers, detection |
| `zf_core.c` | errors, buffered reading, CRC-32, UTF-8, Windows-1252, entities |
| `zf_xml.c` | a pull tokenizer for the XML in Office files and EPUBs |
| `zf_zip.c` | zip: reading through the central directory, stored writing |
| `zf_em.c` | the readers' paragraph emitter: lazy paragraphs, whitespace, encodings |
| `zf_docw.c` | the six document writers |
| `zf_docx.c` | docx and odt readers |
| `zf_rtf.c`, `zf_html.c`, `zf_epub.c`, `zf_text.c` | rtf, html, epub, md and txt readers |
| `zf_tab.c` | tables: csv, tsv, zss, xlsx, ods, both ways; formula translation |
| `zf_arch.c` | zip, tar, gzip extraction |
| `zf_pack.c` | zip, tar, tar.gz writing: deflate (LZ77, fixed Huffman codes), gzip, ustar, zip's directory |
| `zf_conv.c` | detection and `zf_convert()` |
| `sw/apps/convert/convert.c` | the program: fs I/O, arena, progress, the shell, the window |
| `sw/apps/convert/pack.c` | a folder walked (`zwalk`) into `zf_pack` |
| `sw/common/zconvert.[ch]` | the caller's side |

The existing format code elsewhere in `sw/common` (`zinflate.c`,
`zgz.c`, `zimg.c`, ...) stays where it is; `zf_zip.c` and `zf_arch.c`
use `zinflate.c` rather than carrying a second inflater.

## Tests

```
python3 sw/common/formats/tests/run_tests.py      (or: make -C sw/apps/convert test)
```

builds the library for the host with sanitizers (`tests/zfhost.c` is a
command-line converter over stdio) and checks, 437 checks in all:

- **documents** -- fixtures made with python-docx, odfpy and ebooklib,
  and by hand (rtf, html, md; txt in UTF-8, UTF-16 and Windows-1252),
  converted to every document format; each result read back here and
  compared, docx/odt also read by python-docx/odfpy, and docx, odt,
  rtf and html opened by LibreOffice and their text compared; documents
  *written by* LibreOffice from the same source read here;
- **tables** -- xlsx, ods, csv, tsv and zss both ways, read back with
  openpyxl and LibreOffice, formulas kept or turned into values as
  described above, limits applied;
- **archives** -- zip, tar (with long names), tar.gz and gz extracted
  and compared byte for byte; hostile paths refused with nothing
  written; a zip bomb stopped;
- **packing** -- a tree (long names, UTF-8, empty files and folders,
  text, zeros, random bytes) packed into zip, tar, tar.gz and tgz, with
  the compressor and without (a heap too small for it), read back by
  Python's zipfile, tarfile and gzip, by `unzip -t`, `tar tf` and
  `gzip -t`, and extracted again by this library; deflate round trips
  of repeats, near-random and empty input; and `pack.c` itself over a
  host directory (`sw/apps/convert/tests/test_pack.py`): a folder of
  pages of entries, an archive written inside the folder it packs, too
  deep, cancelled at many points with nothing left open;
- **hostile input** -- every fixture truncated and corrupted at many
  points: a clean error or a result, never a crash;
- **cancelling** -- stops at the first poll, from every kind of input.

LibreOffice checks are skipped when it is not installed (or with
`--no-lo`).

## Not yet

- Packing several files, or a selection: a folder at a time.
- Dynamic Huffman codes in deflate, which would bring packing to
  `gzip`'s sizes.
- A cancelled or failed extraction keeps the files it had finished
  (the one being written is removed): the folder holds part of the
  archive, and says so by what is missing.
- Compressed docx, xlsx, odt and ods: their zips are written stored
  (zf_zip.c), though zf_pack.c's compressor could serve; images;
  tables inside documents as tables; more than the first sheet of a
  workbook.
