# Staying responsive

Zeitlos is a responsive system: **an app answers a key, a click or a
message from wm at once, whatever it is doing.** Anything that takes
long enough to notice -- reading a large file, decoding a picture,
rendering a page, printing -- shows how far it has got and can be
stopped instantly (Esc), leaving the app as it was before.

The kernel schedules pre-emptively, so a busy app never stops the
*system*; but an app that is in a long call is not reading its own
messages. Its window does not redraw (wm waits for its redraw
acknowledgement), its keys queue up, and nothing can stop the work. So
the rule is for apps: **the main loop never makes a call that can take
longer than about 20 ms.**

## The shape of a main loop

```c
for (;;) {
    while (z_msg_read(&m) == Z_OK) handle(&m);    // everything waiting
    if (work_pending()) {
        work_step(Z_TICK_HZ / 50);                  // ~20 ms of it, then back
        show_progress();                            // a few times a second
        continue;                                   // no waiting while busy
    }
    z_proc_wait(Z_TICK_HZ / 30);                    // idle: sleep
}
```

Stopping is then simply not calling `work_step()` again and freeing what
the work holds. Long work is one of two kinds.

**Work that is already a loop over pieces** -- copying a file a chunk at
a time, rendering a document a band at a time, sending a print job --
becomes a state machine: what the loop's locals were goes into a struct,
and each step does a few pieces. Nothing else is needed.

**Work that is one long call** that cannot be cut up without rewriting
it -- an image decoder is a loop over the whole file with its state
deep in its locals -- runs as a *coroutine* (below): it keeps its own
stack, and hands control back from inside its callbacks.

## zco: coroutines (`sw/common/zco.h`)

```c
static uint8_t stack[16 * 1024];
zco_t co;
zco_init(&co, long_call, arg, stack, sizeof(stack));
while (zco_resume(&co)) { ...answer messages... }   // false once it returned
/* inside long_call, in a callback: */  zco_yield();
/* to stop:  */  zco_abandon(&co);                   // never resumed again
```

On the board a switch saves and loads the callee-saved registers (ra,
sp, s0-s11: 14 words, a few dozen instructions); an interrupt saves
registers to the kernel's own area and runs on the kernel's stack, so a
stack in the app's memory is as good as the process's. On the build
machine it is `ucontext`, so host tests (with AddressSanitizer) run the
same code.

**Abandoning is instant and safe only for work that holds nothing but
memory the caller owns**: no open files, no messages half sent, no
locks, no `malloc` of its own. So the coroutine computes and the main
loop does the I/O -- it reads the file and hands the coroutine each
chunk. The decoders in `zimg` allocate nothing, which is what makes them
a good fit. One coroutine runs at a time and may not start another.

**The stack is a fixed buffer, and it is guarded.** Nothing grows it,
and nothing stops a call chain from running off its bottom into
whatever lies below. So `zco_init()` fills it with a pattern, and every
time the coroutine hands back, `zco_resume()` checks that the lowest
128 bytes (`ZCO_GUARD`) still hold it. If not, the coroutine is
finished there and then -- `co.overflow` set, never resumed -- and
`zco_resume()` returns false as for an ending; the caller treats it as
a failure (zimgjob reports FAILED). That is after the fact: it cannot
undo what the overflow wrote, but it turns "a corruption somewhere,
much later" into a failure at the next yield, in a test.
`zco_stack_used(&co)` is the high-water mark, for sizing a stack from a
test run. Both read the pattern, so a frame that reserves a big array
and writes little of it can step over the guard and make the mark read
low: give a coroutine about **twice** what `zco_stack_used()` reports.
(The squirrel JPEG decode reports 516 bytes of its 16KB.)

`sw/common/tests/zco_sim/` checks it on RISC-V in the simulator (locals
across 321 switches, deep recursion, a JPEG decoded in a coroutine
equal to the same JPEG decoded directly, and a call chain run off a
2KB stack caught at its first yield):

```
make -C sw/common/tests/zco_sim
sim/zsim-headless -n 0 sw/common/tests/zco_sim/zco_sim.bin    # zco: ok
```

## zimgjob: a picture without stopping (`sw/common/zimgjob.h`)

```c
z_imgjob_t job;
z_imgjob_start(&job, "/media/images/squirrel.jpg", my_alloc, my_free);
/* main loop, nothing waiting: */
if (z_imgjob_step(&job, Z_TICK_HZ / 50) != Z_IMGJOB_RUNNING) { ...done... }
show(z_imgjob_permille(&job));                      // 0..1000
/* Esc: */      z_imgjob_stop(&job);                // file closed, memory freed
/* result: */   uint8_t *grey = z_imgjob_take(&job); // job.w x job.h, 0 black
```

The file is read in the main loop, 2 KB per step. `z_img_decode_gray()`
-- unchanged -- runs in a coroutine on a 16 KB stack (it needs about
3 KB). It yields when it needs bytes not read yet, and every 4 rows.
Progress is the share of the file the decoder has consumed. On the
board the tour's 387x256 JPEG takes about 100 slices of about 10 ms each,
so a key waits at most a slice plus the step's budget.

zimg keeps its working memory in one static area, so **a process decodes
one picture at a time**; an app with several pictures queues them
(slide does).

Host test: `make -f sw/common/tests/Makefile.zcanvas imgjob`. It checks
that a picture decoded in slices equals the same picture decoded in one
call (JPEG, PNG, PGM), that stopping it at two dozen points frees
everything and leaves the decoder right for the next picture, and that a
missing file and a file that is not a picture fail cleanly.

## slide

Pictures in [slide](slide_app.md) load this way. Drawing a card never
decodes anything: a picture not loaded yet is queued, and its frame is
drawn with a progress bar. The main loop loads the queue one picture at
a time, redraws each picture when it is done, and shows
`Loading NAME NN%  (Esc stops)` in the status line. **Esc** stops
loading (the current picture and those waiting), and the picture shows a
light cross; **Enter** on it loads it again. A picture that cannot be
read shows a dark cross, or, if it was just placed with `image`, is
taken off the card again with a message.

Placing a picture puts a 4:3 frame on the card at once; when the picture
has loaded, the frame takes the picture's shape around the same centre
(unless it was moved or resized meanwhile).

A picture let go of -- undone, cut, its object re-made by an edit -- is
kept for a while (four pictures, up to 1.2 MB), so it comes back without
being decoded again. In a show, the card on the screen's pictures load
first, then the next card's, so that moving on usually finds them ready;
moving on never waits for them.

A show also draws the **next card** ahead, the same way: render_card()
calls a hook between objects (`render_set_between()`), and in the
show's coroutine that hook yields, so the main loop answers a key
after at most one object, and going on to the next card costs only the
transition. And a click on a control draws only what the click
changed (`act_touch()`), not the card: the areas, after the event,
once.

Printing does not decode on screen: the job names each picture's file,
and the print service decodes it at the size it prints
([zpaper.md](zpaper.md)).

### Into a document

```c
static uint8_t mem[Z_IMGJOB_DOC_MEM_STACK(8 * 1024)];
z_imgjob_start_doc(&job, path, doc, wpl, w, h, mem, sizeof(mem));
/* step, permille, stop: as above;  job.rows_done rows are in doc */
```

For an app that shows a picture in one bit (view, web): zimg's own
ditherer writes into the caller's 1bpp document through
`z_img_decode_rows()` (`z_img_decode()` with a call after each row), so
no grey image is held. The job allocates nothing: the decoder's state,
the 2KB chunk and its stack come out of `mem`, so a stopped job holds
only its file, which `z_imgjob_stop()` closes. On the board the
decoders use at most about 2KB of that stack (PNG; JPEG about 0.5KB),
so 8KB. The host test also checks document mode: sliced equals one
call, and the file is closed at every stop.

## view

Opening a picture shows the window at once; the rows appear as they are
decoded, and the title says how far (`name 40%  Esc stops`). Scrolling
and redraws work meanwhile. **Esc** stops it and the picture that was
there comes back, at its scroll position: the new one decodes into a
second document (38KB) and the two swap when it is done. SVG is still
drawn in one call (zsvg has no row callback to hand back from).

## web

After a picture's fetch, the page stays live while it decodes into its
box (`sw/apps/web/imgdec.c`, document mode): the bar says
`picture 40% -- Esc stops` and Go becomes Stop. Esc or Stop ends it and
the box says `stopped -- click to load`. However a decode ends -- done,
failed, stopped, or dropped by following a link, Back, reload -- the
spool is closed and removed. Pictures still load when their box is
clicked, one at a time: loading every picture on a page by itself would
need several pictures' memory and fetches at once.

## draw

Opening and saving in [draw](draw_app.md) are state machines
(`sw/apps/draw/drawio.c`): 2KB a read or write, ~20ms a step, the title
showing `Loading NAME 45%  (Esc stops)`. A load reads into a second page
and swaps it in only when the whole file is in, so Esc or a damaged file
leaves the drawing as it was (that page is 37.5KB of .bss: the cheapest
way that is right on a 1MB board, where there is no arena). A save
writes NAME.part and renames it when complete; Esc or a failure deletes
the .part and the old file is untouched. During a save the page is
read-only; during a load drawing is ignored; New/Open/Save are ignored
until the job ends; closing stops a load, or closes once a save
succeeds. Host test: `make -C sw/apps/draw test` (stops at every step of
each).

## posix

A command line runs in a coroutine on an 8KB stack, with 2KB of slack
below it checked after every slice, kept in `main()`'s frame
(`sw/apps/posix/job.c`). It hands back from `px_intr()` every ~10ms,
and from the output path while the terminal has not acked. Between
slices the main loop serves every session: echo, editing, output, Ctrl+C,
connects and children. Builtins keep their state in statics, so one
command runs at a time and the others wait their turn, with a note on
their terminal. Ctrl+C stops a running command cooperatively, through
the builtin's own cleanup, or takes a waiting one out of the queue.
Details in [posix.md](posix.md), "How a command runs".

**A coroutine that calls `malloc()` must not have its stack from
`malloc()`** (or anywhere in the heap's way): newlib's `_sbrk()` refuses
to grow the heap past the current stack pointer, so with the stack in
the heap every `malloc` inside the coroutine fails. posix keeps its
command stack in `main()`'s frame for that reason. A coroutine that
allocates nothing (zimgjob's decoders) can have its stack anywhere.

## Still to do

| App | The long call | The way out |
|---|---|---|
| `view`, `web` | SVG, drawn in one call | a row or element callback in zsvg to yield from |
| `draw` | a brush or eraser stroke across the whole page in one mouse sample (~50ms) | stamp a bounded number of points per step |
| `sheet` | recalculating a 4,096-cell sheet (to be measured on the board) | a coroutine around the evaluator, or a generation-stepped recalc |
