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

`sw/common/tests/zco_sim/` checks it on RISC-V in the simulator (locals
across 321 switches, deep recursion, a JPEG decoded in a coroutine
equal to the same JPEG decoded directly):

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

Printing does not decode on screen: the job names each picture's file,
and the print service decodes it at the size it prints
([zpaper.md](zpaper.md)).

## Other apps

These still make long calls in their main loops and should move to the
pattern above:

| App | The long call | The way out |
|---|---|---|
| `view` | `z_img_decode()` of the whole picture, dithered to the window | the same coroutine as zimgjob, with the dithering row callback; the rows drawn so far show the progress |
| `web` | `z_img_decode()` of each picture on a page | queue them as slide does; the page lays out with placeholder boxes first |
| `draw` | reading and writing the canvas file in one loop | a state machine: a chunk a step, with a progress bar |

zimgjob could take a "dither into this window" mode for `view` and
`web`; until then each can run its own coroutine the way zimgjob.c does
(about 150 lines).
