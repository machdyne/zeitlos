# print: the print service

`sw/apps/print`. Prints what an app hands it -- to a printer on the
network, or to a PDF or PostScript file. `slide` is the first app that
prints ([slide_app](slide_app.md#printing)); the API is open to any
app ([zprint](#printing-from-an-app)).

```
print                    ask the printer in zeitlos.cfg what it takes
print ipp://10.0.0.5/ipp/print    ... or that one
print -j JOB -c PID:SERIAL        (how apps start it)
```

## Using it

Set the printer once, in `zeitlos.cfg` (or `settings`):

```
apps.print.printer: ipp://192.168.1.50/ipp/print
```

then run `print` alone: it asks the printer what it is and what it
takes, and shows the answer until **OK** (or Esc, Enter, the close
box). If that works, printing will.

From `slide`, **Ctrl+P** prints the deck. A small window shows what
print is doing -- the sheet it is on, waiting while the printer is
busy -- with **Cancel**, and how it ended: *Printed: 7 sheets, sent as
PDF*, or what went wrong. It closes by itself a few seconds after
success; after an error it waits for **OK**.

Printing runs in its own process: the app goes on working meanwhile,
and a print job finishes even if the app is closed.

## Setting up

| Setting | Default | |
|---|---|---|
| `apps.print.printer` | (none) | the printer's address |
| `apps.print.paper` | `letter` | the paper when a job does not name one: `letter`, `a4`, `legal`, `a5`, `a6`, `4x6`, `5x7`, `3x5` |
| `apps.print.format` | `auto` | to an IPP printer: `auto`, `pwg`, `pdf` or `ps` |
| `apps.print.trays` | (none) | which tray holds which paper: `a4 main; a5 rear labels` ([Trays](#trays)) |

**The address** is one of:

| Address | For |
|---|---|
| `ipp://ADDRESS/ipp/print` | a printer that speaks **IPP** ("driverless", "IPP Everywhere", "AirPrint"): nearly every network printer made since about 2013, inkjet or laser -- a Canon PIXMA, an HP, a Brother, an Epson |
| `ipp://ADDRESS:PORT/PATH` | the same at another port or path (`/ipp/print` is the standard one) |
| `socket://ADDRESS` | a printer that takes **PostScript** on port 9100 ("raw", "JetDirect"): a PostScript laser such as a LaserJet |
| `ADDRESS` alone | the same as `ipp://ADDRESS/ipp/print` |

ADDRESS is an IP address or a name DNS can find. A printer shows its
address on its own panel (a PIXMA: *Settings > LAN settings > Confirm
LAN settings*), and the router lists it among its clients.

**No drivers.** A driverless printer says what it takes when asked
(*Get-Printer-Attributes*), and print makes that: there is nothing to
install for a model. That is also how Linux prints to a PIXMA.

### Trays

A printer with more than one paper source -- a PIXMA's cassette
(`main`) and rear feeder (`rear`) -- takes each sheet from the tray
that holds its paper. print chooses the tray for a job, first match
wins:

1. the job's own tray: slide's `/print tray rear` (kept in the deck
   with `/print setup tray rear`), or any app's `media-source` option;
2. **`apps.print.trays`**: pairs of paper and tray, separated by `;`
   or `,`, with the paper's kind after the tray if it matters --
   `a4 main; a5 rear labels`. A job on A5 goes to the rear tray as
   labels, one on A4 to the cassette;
3. what the printer says is loaded where (IPP's *media-col-ready*), for
   a printer that knows -- many know the cassette's size, set on the
   panel, but not what is in the rear feeder;
4. otherwise the printer's own choice.

`print ADDRESS` (or `print` alone) shows the printer's trays by their
IPP names (`main`, `rear`, `manual`, `by-pass-tray`, `auto` ...), what
it says is in them, and the `apps.print.trays` setting. The finished
job says which tray it went to: *Printed: 1 sheet, sent as PDF, from
rear*.

A PIXMA with A4 in the cassette and A5 labels in the rear feeder:

```
apps.print.paper: a4
apps.print.trays: a4 main; a5 rear labels
```

and a deck of A5 labels (`/page a5`) prints from the rear, everything
else from the cassette. A label deck on A4 paper that should still come
from the rear says so itself: `/print setup tray rear`.

## What goes to the printer

| The printer takes | print sends | |
|---|---|---|
| PDF (over IPP) | **PDF** | the fastest by far: the printer draws it at its own resolution |
| PWG raster, not PDF | **PWG raster**, grey, 300 dpi (or the nearest it has) | every IPP Everywhere printer takes it; print draws every dot |
| PostScript only | **PostScript** | |
| (a socket printer) | **PostScript** | |

`apps.print.format` (or a job's `format` option) asks for one of
them: `pwg` if a printer's PDF comes out wrong, `ps` for a PostScript
printer that also speaks IPP.

Everything prints in **grey**, as it is on the screen: shades as real
greys and pictures as grey photographs, which the printer halftones
far better than a pattern of dots made here would look. Patterns print
as patterns, at their size on the screen (one bit a card unit).

**A busy printer** -- printing someone else's job -- is waited for:
the window says *Waiting: the printer is busy* and tries again every
five seconds, for up to half an hour, or until **Cancel**.

## Printing from an app

`sw/common/zprint.h`. The app describes its pages through
[zpaper](zpaper.md) -- with its own drawing code, onto a recording
target -- into a job file in `/tmp`, and print takes it from there:

```c
#include "../../common/zprint.h"

z_print_t pr;
if (z_print_begin(&pr, "My document")) {
    zp_job(&pr.w, "nup", "2");               // options, before the pages
    for (int i = 0; i < pages; i++) {
        zc_xf_t xf;
        zc_target_t *t = zp_page_begin(&pr.w, &page_shape, &xf);
        draw_page(t, &xf, i);                // the window's own code
        zp_page_end(&pr.w);
    }
    z_print_end(&pr);                         // runs print on the job
}
```

and `zprint.o zpaper.o` in the app's OBJS. That is all an app needs:
print shows its own window, and its progress and outcome come back as
`Z_PRINT_PROGRESS` (`tag` 0..1000) and `Z_PRINT_DONE` (`tag` 0 or a
`Z_PRINT_E_*`), for an app that wants them (`z_print_msg()`). The
subjects are block 472 (`zsubjects.h`).

A picture from a file is best given by path (`zp_image_file()`): print
decodes it at the size it prints, and the job stays small.

## Job options

`key=value`, each a `JOB` record before the first page (`zp_job()`).

| Option | Values | |
|---|---|---|
| `title` | text | the job's name on the printer, and the PDF's title |
| `dest` | (empty), `pdf:/PATH`, `ps:/PATH`, or an address | where: the printer in `zeitlos.cfg`, a file, or another printer |
| `paper` | `letter`, `a4`, ... `page` | the sheet; `page`: each sheet the size and shape of its page (a PDF of slides). Default: the pages' own paper if they are paper-shaped, else `apps.print.paper` |
| `nup` | 1 .. 16 | pages to a sheet |
| `size` | `fit`, `actual` | as large as fits, or the page's own paper size |
| `cut` | `1` | cut marks between the pages |
| `frame` | `1` | a hairline round each page |
| `gap` | tenths of a mm | the gutter between pages (8 mm with several to a sheet) |
| `margin` | tenths of a mm | at least this from each edge (5 mm); a printer's own margins win if larger |
| `pages` | `3`, `2-5` | which pages |
| `copies` | 1 .. 99 | |
| `quality` | `draft`, `normal`, `high` | IPP print-quality |
| `media-type` | `stationery`, `photographic` ... | the paper's kind, for a printer that cares |
| `media-source` | `main`, `rear`, `manual` ... | the tray |
| `format` | `auto`, `pwg`, `pdf`, `ps` | overrides `apps.print.format` |

## Laying pages on sheets

[zpage](zcanvas.md#pages) decides:
- **fitted**, a page is as large as the printable area allows, and
  turned a quarter if that makes it larger: a landscape slide on
  portrait Letter lies sideways, and its patterns, pictures and text
  turn with it;
- **actual size** prints a paper-shaped page at its own size (a 3x5
  card on Letter is 3 by 5 inches, with cut marks to trim it);
- **several to a sheet** go in the grid of columns and rows that makes
  them largest, upright, with a gutter; four 16:9 slides to an A4
  sheet are a column of four;
- **margins** are the larger of the job's and the printer's (which it
  reports for its paper; a PIXMA's bottom margin is wider than its
  others).

A sheet goes to the printer portrait, as it feeds, named by its PWG
media name (`na_letter_8.5x11in`, `iso_a4_210x297mm`,
`na_index-4x6_4x6in`).

## Speed

Measured on the RISC-V CPU, under `sim/zsim-headless` (which counts
instructions), for the seven-card slide tour on Letter
(`sw/apps/print/bench`):

| | instructions | at 48 MHz, ~12M instructions a second |
|---|---|---|
| the whole tour as **PDF** | 9.3M | under a second |
| the whole tour as **PostScript** | 12.9M | about a second |
| one sheet as **PWG raster**, 300 dpi: text and shapes | 42-80M | 4-7 s |
| one sheet as PWG raster: a photograph on it | 132M | 11 s |

A raster sheet is 2550 x 3300 grey dots, every one drawn here; a PDF
page is a few KB of drawing instructions and the printer draws the
dots. That is why `auto` prefers PDF. Either way a job goes out as it
is made -- a sheet is sent while the next is drawn -- and is never
held whole: the printer starts on the first sheet while the last is
still being drawn.

What made the raster path this fast (from 922M to 529M instructions
for the tour; `sim` with a per-address profile found each):
- a picture drawn turned is sampled bilinearly in 32 bits, with no
  call a pixel (was 28% of the time);
- the PWG coder compares lines a word at a time and holds the line it
  repeats instead of copying it, and band rows are a whole number of
  words apart so every line is word-aligned;
- pattern fills step cell by cell along a row instead of dividing for
  every pixel;
- fills are written a word at a time;
- glyphs come from the glyph cache on grey bands too.

The transfer: the tour is 210KB as PDF and 1.9MB as PWG raster
(blank lines and white margins cost nearly nothing).

## Memory

print asks the arena for 4MB, and makes do with 2MB or 1.5MB:
- the job (the tour: 21.7KB);
- a band of 64 lines of the widest sheet (160KB at 300 dpi);
- the fonts the job names, loaded from `/sys/fonts` once;
- pictures, decoded to grey at most once each (640x480 at most, as
  zimg decodes them -- about 160 ppi across a 4 inch photo, which the
  printer smooths), kept within a budget and decoded again if dropped;
- the glyph cache (192KB), and the PDF writer's tables.

The app that prints spends only the job file: a few KB of vectors
written to `/tmp`.

## Inside

```
app ── zpaper job in /tmp ──> print ─┬─ run.c: probe, choose, send
                                     │
          zpaper.c player ── raster.c (bands) ── pwg.c ──┐
                             vec.c (PDF, PostScript) ────┤── ipp.c (HTTP, IPP) ── net's socket
                                                         └── a file
```

| File | Does |
|---|---|
| `sw/common/zpaper.[ch]` | the job format: recorder, reader, raster player ([zpaper](zpaper.md)) |
| `sw/common/zprint.[ch]` | the app's side: job file, starting print, its messages |
| `job.c` | options; which pages go on which sheet, where |
| `raster.c` | a sheet in bands of grey lines |
| `pwg.c` | PWG raster |
| `vec.c` | PDF and PostScript, one writer |
| `ipp.c` | addresses, HTTP/1.1 (chunked), IPP requests and answers |
| `res.c` | fonts by name, pictures by path, decoded once |
| `run.c` | a job from start to end, over any transport |
| `print.c` | the board: the window, net's socket, the caller |

**PWG raster** (PWG 5102.4): "RaS2", then for each sheet a 1796-byte
header and its lines, each a repeat count and the line in PackBits by
pixels. `sgray_8`, or the same grey as `srgb_8` for a printer that
takes only colour.

**PDF** (1.4) and **PostScript** (level 2) share their drawing: the
PostScript prolog defines PDF's operator names, so a page's content is
the same text in both. Coordinates are a tenth of a point.
- **Text stays text.** Each face is a Type 3 font of the glyphs the job
  uses: a Hershey glyph as its strokes (round pen, the same width as
  on the screen), a bitmap glyph as an image mask. A page of text is a
  few KB, and prints at the printer's resolution.
- **Pictures** are 8-bit grey, run-length coded (both formats decode
  it: no deflate here). In PostScript also ASCII85, so a 9100 printer
  sees only text.
- **Patterns** are tiling patterns, anchored at the page's corner and
  turned with it.

**IPP** (RFC 8010/8011) over HTTP/1.1 on port 631:
- *Get-Printer-Attributes* first: what formats, resolutions, paper and
  margins the printer has, and whether it is busy;
- then *Print-Job*, the document streamed after the attributes in the
  same chunked body, as it is made;
- a printer that answers *busy* is asked again later.

**The transport** is `net`'s raw socket ([networking](networking.md)):
up to eight sends in flight, net holding what the TCP queue cannot
take yet. A socket printer never answers, and closing a socket drops
what net has not yet sent, so after the last byte print waits for net
to have read everything and gives it two seconds more before closing.

## Tests

```
make -C sw/apps/print test       # 845 checks, then the whole path end to end
make -C sw/apps/print/bench && sim/zsim-headless -m 33554432 -n 0 sw/apps/print/bench/bench_print.bin
```

`tests/test_print.c` (ASan, UBSan): PWG lines decode to themselves
(400 random lines), whole pages with repeats and a white end;
addresses; a Get-Printer-Attributes request; a printer's answer parsed,
collections skipped, cut short anywhere without a fault; a Print-Job
with `media-col` and a tray; HTTP responses chunked and not, after a
100 Continue; the request's chunks; job options, page ranges, sheets
and margins; PWG media names; number formatting.

`tests/run_tests.sh`, end to end, each output checked by a program
that is not this one:
- the slide tour, from slide's `/print` code, as PWG raster -- read by
  `tests/pwg_decode.py`, written from the standard, not from `pwg.c` --
  also as `srgb_8`, and four to an A4 sheet with cut marks;
- as PDF: `qpdf --check`, and drawn by Poppler (`pdftoppm`) at 300 dpi
  and compared with our own raster (`tests/compare.py`: within 0.4%,
  with patterns allowed to drift by the 5-against-4.98-dot difference
  in a pattern's cell);
- as PostScript: drawn by Ghostscript and compared the same way;
- `paper=page`: each PDF page the card's own shape;
- over IPP to **ippeveprinter**, CUPS's reference IPP Everywhere
  printer: the probe (with its trays and what it says is loaded in
  them), then Print-Job as PDF (what `auto` picks),
  PWG raster and PostScript, the second and third waiting while the
  printer is busy with the first; the three documents it kept are
  checked as above. (On a build machine without IPv6 ippeveprinter
  will not start; `tests/no_ipv6.c` makes it. It also wants DNS-SD:
  `dbus-daemon --system --fork; avahi-daemon -D` in a container.)
- to a port-9100 socket: the PostScript arrives byte for byte.

## Not yet

- **Discovery**: a printer's address is typed in; finding printers by
  DNS-SD (Bonjour) needs multicast DNS in `net`.
- **IPPS** (IPP over TLS): plain IPP only. The TLS in `web` could serve.
- **URF** (AirPrint's raster), for a printer that takes neither PWG
  nor PDF; **PCL**, for a laser without PostScript.
- Colour, two-sided, and following a job once the printer has taken it.
- Printing from `text`, `read` and `draw`: zprint is ready for them.

## On the board

To check in the final hardware phase:
- `print` alone, against the PIXMA: it answers, and says what it takes;
- Ctrl+P in slide: the tour prints, as PDF if the PIXMA takes it, and
  with `apps.print.format: pwg` as raster; the margins, the turned
  cards, the photograph;
- `/print 4up cut paper a4`; a 3x5 deck with `/print actual cut`, and on card stock;
- `/print pdf`: a file that opens on a PC;
- Cancel while drawing, and while waiting for a busy printer;
- a LaserJet at `socket://`: the whole job prints (nothing lost at the
  close);
- the times in "Speed", with the cycle counter.
