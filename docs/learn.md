# learn

*Design, draft 2, 2026-10-26. Built so far: the course format and its
checker (L1, [learn_format.md](learn_format.md)). With the
[curriculum map](https://github.com/machdyne/ark/blob/main/edu/curriculum.md) (in the Ark) and the [screens](learn_ui.md),
this is the plan of a long project; its decisions are
`claude/real-world-proposal.md` in the project.*

`learn` is a school on the machine: courses from kindergarten through
the first years of college, real-life courses (survival, first aid,
money, conflict), languages, and courses on Zeitlos itself. It is
graphical -- lessons draw their own figures, live, in Scheme -- and it
runs with the network unplugged, from files on the card.

It has three jobs, in this order:

1. **It must be accurate.** Teaching something false is worse than
   teaching nothing. Everything below about formats and review exists
   to make a course's claims checkable, checked, and correctable.
2. **It must actually teach**, using methods with good evidence behind
   them, built into the format so that every course uses them whether a
   person or a model wrote it.
3. **It must be fast and obvious.** One key moves on; nothing to learn
   about learn before learning the subject.

## How a lesson teaches

The method is fixed by the app and the format, not left to each
author. It follows the techniques the research on learning rates most
useful -- practice testing (retrieval) and spaced practice above all
([Dunlosky et al., 2013](https://www.psychologicalscience.org/news/releases/which-study-strategies-make-the-grade.html)) --
and the shape of a good lesson as Rosenshine summarised it from
classroom research ([Principles of Instruction](https://www.aisnsw.edu.au/teachers-and-staff/teaching-and-learning/literacy-and-numeracy/foundations-of-effective-instruction/principles-of-instruction)):

| What the research says works | What learn does |
|---|---|
| Begin with a short review of earlier learning | A lesson opens with a warm-up of up to three questions from its course that are due for review, chosen automatically ([below](#review)) |
| Present new material in small steps, with practice after each | A lesson is steps of one idea each; a check follows every step or two; the format allows no long step (`learncheck` enforces a limit) |
| Retrieval practice: recalling, not rereading | Checks ask the learner to produce or choose an answer before seeing it; review is questions, never rereading |
| Spaced practice | Every question answered goes into a review queue of five Leitner boxes (1 day to 35 days); `cards` will too |
| Interleaving: mixing kinds of problem | Review mixes questions from different units and courses |
| Worked examples, then fading them | `example` steps show a solution step by step; the next asks for the last step, then the last two, then all of it |
| Immediate, specific feedback | Each wrong choice has its own explanation, written for the misconception that leads to it; the learner then gets a new question of the same kind, not the same one again |
| Words and pictures together | Every teaching step can have a figure beside its text; figures are drawn, so they can move and respond |
| Mastery before moving on | A unit is mastered at 80% or better on its check; the next unit waits (a learner can always choose to go on, and can test out of a unit) |
| Concrete, then pictures, then symbols (school math) | A math course's figures go from objects to bars and number lines to notation, unit by unit |
| Systematic phonics for beginning readers | `ela-k`..`ela-2` and `life-read` teach letter-sound correspondences in a fixed sequence, with decodable texts |

Hints are a ladder: a nudge, then a bigger one, then the worked
answer. A question answered after the last rung counts as not yet
known and goes into review.

learn does not use streaks, points, reminders or notifications. It
records honest progress, shows it when asked
([learn_ui.md](learn_ui.md#progress)), and otherwise stays out of the
way ([philosophy.md](philosophy.md#tool)).

## Two faces

learn is a **dual app** ([app_kinds.md](app_kinds.md), the pattern of
`poker` and `chess`): one process, one window, two faces.

- **The desktop face** is where most courses are read: the 640x480
  desktop, one bit a pixel, crisp bitmap text for reading and Hershey
  fonts for headings and formulas. The text is on the left and the
  **figure pane** on the right. The pane is at most 320x240, which is
  exactly what the colour window can show: on a board built with
  `COLOR_WINDOW`, the focused lesson's figure is in colour (8 colours
  with six game planes, 32 with eight) and the text stays sharp in one
  bit ([color.md](color.md#the-colour-window)). Elsewhere the figure
  is drawn in black, white and patterns, and every figure is written to
  read that way too.
- **The screen face** is the arcade runtime's 320x240 screen: colour in
  a window, and full screen pixel-doubled (16, 64 or 256 colours by
  board). It is for the youngest learners -- large type, big targets for
  a mouse or a gamepad, speech -- and courses for K-2 open on it, full
  screen. Any lesson can be shown on it (F10), laid out again for
  320x240.

The colour figure on the desktop face is new: today the colour window
belongs to a screen app's whole window, and [app_kinds.md](app_kinds.md)
says colour is never shown on a desktop face. wm needs no change for it
-- it gives the colour window to the focused window that has
`Z_WIN_FLAG_COLOR` and asks for it, whichever face it shows -- so learn
can take it for its figure pane the way `colwin` does: ask for it only
while a colour figure is on screen, set it to the pane while the pane is
wholly uncovered, and draw the figure's planes. One cost on the 45F
boards: desktop colour uses the same colour window over the whole
screen, so while a lesson with a colour figure is focused the desktop's
four colours are off (they come back when it loses focus). Text-only
steps do not ask, and so do not cost it.

### Windows and the dock

learn opens **maximized** (`z_win_maximize()`, new with learn): the dock hides, as it does for any maximized
window ([window_manager.md](window_manager.md#maximize-and-shade)), and
the lesson has the whole screen. It is an ordinary resizable window:
Alt+Equal or a double-click on the titlebar gives it back its size and
brings the dock back, and learn lays itself out again (below about 640
wide the figure goes above the words instead of beside them; the pane
stays at most 320x240). Other apps are reached without the dock in two
ways: a lesson brings the app it needs (`try` steps open it beside the
lesson), and the dock hides per workspace, so the next workspace
(Super+2) is an ordinary desktop. A child's dock can be made to hold
only learn and the tools a lesson uses ([dock.md](dock.md)); learn
itself never locks anyone in.

## The lesson format

### What was considered

| | `slide` decks (`.zsl`) | Markdown | Something new |
|---|---|---|---|
| Graphical | yes: shapes, controls, Scheme | no: text, and pictures as files | -- |
| Layout | absolute: objects at coordinates on a 512-unit page | flows: the app lays it out | -- |
| Different sizes (a window, 320x240, paper) | the card scales; text does not reflow | reflows | -- |
| Written by a model, thousands of times | coordinates are where generated content goes wrong (overlaps, text out of its box), and no check catches all of it | what models write best; easy to check | must be taught to every author |
| Readable without the app | as S-expressions | as plain text, in `read`, on paper | -- |
| Indexed by `ask` | no | yes (ask already indexes Markdown) | -- |
| Questions, feedback, review | buttons and Scheme for each | none | -- |

Neither alone is right. Slide is graphical but positional, which is the
wrong thing to generate at scale and the wrong thing to show at three
sizes; Markdown flows and is easy to write and check, but knows nothing
about questions or drawing.

### What is proposed: Markdown for words, S-expressions for everything else

A lesson is a Markdown file. Prose is Markdown, laid out by learn for
whatever face and size it is shown at. Everything that is not prose is
a fenced block whose contents are S-expressions, read by the same
reader as `.zsl`:

- **data** blocks (`check`, `cards`, `example`, `order`, `match`,
  `try`, `note`, `warn`, `sources`) are read, never evaluated, like a
  deck file -- so a question is a question, and `learncheck` can verify
  its answer without running anything;
- **`figure`** blocks are Scheme, run by learn to draw, in a small
  sandbox (below).

A hand-drawn figure can still come from `slide`: a `figure` can name a
card of a `.zsl` deck, so slide is the drawing editor for figures that
are better drawn than computed (a heart, a shelter, a knot).

The source of the step in [learn_ui.md](learn_ui.md#a-teaching-step)
would be:

````
# Same denominator

```lesson
(objective "Compare two fractions with the same denominator.")
(standards "3.NF.A.3d")
(minutes 20)
```

When two fractions have the same denominator, the whole is cut into
pieces of the same size.

Then the fraction with more pieces is the greater one.

$$ 3/4 > 2/4 $$

The bottom number, the denominator, tells the size of the pieces.
The top number counts them.

```figure
(whole-bar   0  "1")
(frac-bar    1  4 3 'warm "3/4" "3 pieces")
(frac-bar    2  4 2 'cool "2/4" "2 pieces")
(caption "Same size pieces: count.")
```

---

```check
(choice
  (ask "Which is greater?" (math "3/5") "or" (math "4/5"))
  (right "4/5 is greater")
  (wrong "3/5 is greater"
    "Both are fifths, so every piece is the same size. 4 pieces are more than 3.")
  (wrong "They are equal"
    "The pieces are the same size, but there are 4 of them in one and 3 in the other.")
  (hint "Picture both bars. Which has more pieces shaded?"))
```
````

`---` separates steps. `$...$` and `$$...$$` are formulas in a small
notation that learn typesets: `a/b` as a stacked fraction, `x^2`,
`x_1`, `sqrt(L/g)`, `pi`, `theta` and the other Greek letters by name
(the Hershey Greek face is in the tree), `<=`, `!=`, `*` as a times
sign. No TeX: the notation is meant to read correctly as plain text too.

### Blocks

| Block | What it is |
|---|---|
| `lesson` | the lesson's objective, standards, minutes, needs |
| `figure` | Scheme that draws in the figure pane |
| `check` | a question: `choice` (one or several right), `number` (with tolerance and units: `(number 2.01 (within 0.01) (unit "s"))`), `fraction` (exact, equivalent forms accepted or not), `text` (accepted answers, normalised; accents strict or "nearly"), `kana` (romaji or kana), `order`, `match`, `point` (click a place on the figure) |
| `example` | a worked example, step by step, faded on later uses |
| `cards` | flashcards (front, back, optional figure or sound); they feed review |
| `try` | do something in a real app beside the lesson -- `(try basic ...)`, `(try sheet "budget.csv" ...)` -- with a check that decides when it is done |
| `say` | words spoken aloud (pre-readers; also any step with S) |
| `note`, `warn` | an aside; `warn` for safety (it is drawn with a heavy border and can never be skipped unread) |
| `sources` | where the step's claims come from (below) |

### Figures

A figure is Scheme ([scheme.md](scheme.md)) that draws in a **320x240
unit space** -- the screen face exactly, scaled to the pane elsewhere,
and to the page when printed (through zpaper, like slide). It sees:

- drawing: `line`, `rect`, `circle`, `poly`, `arc`, `text`, `math`
  (typeset formula), `arrow`, `image` (a picture in the course);
- **colours by role**: `'ink`, `'paper`, `'warm`, `'cool`, `'good`,
  `'bad`, `'accent`, `'sky`, `'earth`, ... Each role has a colour for
  the colour window and the screen face, and a pattern for one bit, so
  a figure is right in both without being written twice;
- **controls**: `(slider 'length 0.1 2.0 0.5)`, `(drag 'point x y)`,
  `(toggle 'show-forces)`; the figure is drawn again when one changes;
- **time**: `(every ms ...)` for animation;
- the course's **figure library** (`lib/*.scm` in the pack: number
  lines, fraction bars, axes and plots, circuits, maps, clocks,
  molecules, ...), tested once and used everywhere, so a lesson says
  `(frac-bar 1 4 3 'warm)` rather than computing rectangles.

It does **not** see files, the network, other programs or the
learner's records: a course from a pack can draw, compute and animate,
and nothing else. A figure runs in slices like everything else in
Zeitlos ([responsive.md](responsive.md)): drawing a frame that takes
too long makes the figure drop frames, never the window stop answering.

## Packs

Courses come in **packs**: folders of courses at `/opt/ark/edu*/` on the
card. learn's catalogue reads every pack whose name starts with `edu`,
then a folder `edu` on a USB stick. A course found twice counts once:
the first pack in name order wins, then the stick.

| Pack | Holds | Lives in | Built by | Ships |
|---|---|---|---|---|
| `edu-zeitlos` | the `zeit-` courses (14): Using learn; Getting started; Timeless Computing ([tc.md](tc.md), a unit a chapter); Writing; Records and numbers; Making things; Keeping your work; The network; Programming in BASIC, in Scheme, C on Zeitlos; Inside Zeitlos; Connecting things; The FPGA | this repo, `sw/data/learn/edu-zeitlos/` | `tools/learn/pack.py` | with the base set; updated with the system |
| `edu`, `edu-k8`, `edu-life`, ... | everything else on the Ark's curriculum map ([`edu/curriculum.md`](https://github.com/machdyne/ark/blob/main/edu/curriculum.md)) | the Ark repo, `edu/<course-id>/` | `tools/learn/pack.py` from the ark clone (`tools/learn/packs/edu.cfg`: `repo = ark ...`, `source = @ark/edu`) | the `edu` set, when asked for: `ZEITLOS_ASK_PACKS="... edu"` on the release, `update -s edu` on the board |

**Why two places.** The Zeitlos courses describe the system: its keys,
its apps, learn itself. So they have to change in the release that
changes those, and they ship with it, as `zdocs` does. Courses about
fractions or German change on their own schedule and go through their
own review, so they live with the Ark's other offline libraries. The
`zeit-` course ids belong to edu-zeitlos; the pack tool refuses them in
any other pack.

**A pack on the card** is laid out like this:

```
/opt/ark/edu-zeitlos/
  pack.cfg                what was built: name, version, its courses and their versions, a hash
  lib/*.scm               optional: figure procedures every course of the pack can use
  zeit-learn/             a course, exactly as written (learn_format.md)
    course.cfg
    01-start/...
```

The courses are copied as they are, because learn reads the source
format itself. A figure sees learn's built-in library, then the pack's
`lib/`, then the course's own `lib/`
([learn_figures.md](learn_figures.md#how-a-figure-runs)).

**Building one.** A recipe names the pack, its version, where its
courses are and which go in (`tools/learn/packs/edu-zeitlos.cfg`).
`./tools/learn/pack.py build <recipe>` then:

1. runs learncheck on every course, with `--release` when the recipe
   asks for it; an error stops the build;
2. checks every card path the way FatFs on the board will open it;
3. writes `tools/learn/out/<pack>/opt/ark/<pack>/` exactly as the card
   will have it.

The same sources make the same bytes. `release/` ships it like an ask
pack (`release/lib/mkfatimg.py`, `pack_layout()`): edu-zeitlos is in
every image and in the base set's manifest, so `update` replaces it
with the system. A course pack has no ask index yet; adding one is L5.

**The Ark's packs** will be built by the same tool, from the ark clone
`tools/ask` already keeps (`source = ark:edu` in a recipe), with
`--release` on. The tiers are recipes that pick courses.

**Licences.** The courses are CC0. The few things that cannot be (a
CC BY-SA dictionary, stroke-order data) go in separately licensed
packs, never mixed in.

**The learner's records** are a folder per learner. See "Learners"
below.

A course's `course.cfg` -- `key = value` lines as in `zeitlos.cfg`,
but learn's own keys, and a repeated key is a list
([learn_format.md](learn_format.md#coursecfg)):

```
id = math-3
title = Grade 3 math
level = 3
area = school
class = A
version = 3
format = 1
author = Machdyne
author = Claude (Anthropic model)
reviewer = <name>; <qualification>; 2026-11-02
standards = CCSS grade 3
needs = math-2
```

Its parts, units and lessons are its folders and files, in the order
of their numbers.

## Learners

Zeitlos is a single-user system, and learn keeps it that way: a
**learner** is a name and a folder of records, not an account. There is
no password and no protection between learners -- a profile is for
keeping progress apart, not for keeping it secret.

- **On the machine**: `/data/learn/learners/<name>/`, one folder each.
  This is learn's own data ([layout.md](layout.md): an app writes to
  `/data/<app>/`), and `backup` copies `/data` by default, so a parent
  or teacher who backs up the machine keeps every learner's progress
  ([backup.md](backup.md)). A family of one to six on one shared
  computer is the case this is for.
- **On a USB stick**: `/usb/learn/<name>/`, the same folder. In a
  school, each learner carries their own stick: plug it in, and learn
  offers that learner; take it out, and nothing of theirs is left
  behind. A learner on the machine can be copied to a stick and back.
  learn writes to the stick only what the learner did (a lesson
  finished, a review answered), each file whole or not at all
  (`.part`, then renamed), so pulling the stick out loses at most the
  step in progress.
- Every record is a plain text file a person can read, copy or delete
  without learn.

## Accuracy

Every course passes these steps before it is shipped, and its
`course.cfg` records who did each:

1. **Written against the map.** The course's units, level and standards
   come from the Ark's curriculum map ([`edu/curriculum.md`](https://github.com/machdyne/ark/blob/main/edu/curriculum.md)); the outline is
   reviewed before lessons are written.
2. **Sources.** A claim of fact that is not common knowledge at the
   course's level cites a source in a `sources` block -- preferably one
   in the Ark library, so the reviewer and the learner can read it on
   the machine. Time-sensitive claims (class C) carry an "as of" date.
3. **Mechanical checks** (`learncheck`, on the build machine and on
   Zeitlos):
   - the format: every block parses; steps are short; every `check` has
     a right answer and every wrong answer has feedback;
   - **answers are computed, not trusted**: a `number`, `fraction` or
     `choice` whose right answer is arithmetic is evaluated, and a
     figure's labels are checked against the values it draws from
     (a bar labelled 3/4 that shades 2 of 4 fails);
   - every figure runs, in the sandbox, at every size, and its output is
     drawn for a reviewer to look at;
   - reading level against the course's level; links and pictures exist;
     every picture has a source and a licence.
4. **An independent fact-check.** Each claim is checked against its
   source by someone (or a separate model run, with no access to how
   the lesson was written) other than its author; disagreements go to a
   person.
5. **Human review**: one reviewer for class A, a qualified one for
   class B, more than one for class D. Class B courses are not shipped
   until then.
6. **Errata.** A learner who finds a mistake presses **M** on the step;
   the report (course, version, lesson, step, what they typed) is kept
   in `reports.txt` for them to send. A fix is a new course version;
   `course.cfg` counts the errata fixed, and the course screen shows it.

The catalogue shows each course's authors and whether, when and by how
many people it has been reviewed ([learn_ui.md](learn_ui.md#the-catalogue)).

## Writing courses with models

Most courses will be drafted by models. The format is designed for that:
prose in Markdown, questions as data a program can check, figures from
a shared library, and nothing positional. The authoring guide
([learn_authoring.md](learn_authoring.md)) holds the style rules per
level, the prompt for each step of a course (outline, lessons, the
fact-check, fixing), the reviewer's checklist and a first estimate of
what it costs. A model that drafted a course
is listed among its authors.

## Plan

| Step | What | State |
|---|---|---|
| L0 | curriculum map; screens; this design | done |
| L1 | the format in full ([learn_format.md](learn_format.md)), the parser and `learncheck` (`tools/learn/learncheck`), the authoring guide ([learn_authoring.md](learn_authoring.md)) | **done** |
| L2 | `learn`, desktop face: home, catalogue, course, lessons, checks, progress files, speech ([below](#the-app)) | **done** |
| L3 | figures in Scheme, sliders and animation, the figure library ([learn_figures.md](learn_figures.md)); the colour figure pane; the review queue and warm-ups; links | **done** |
| L4 | the screen face for the youngest; `try` steps; the parent and teacher view; worksheets printed through zpaper | |
| P1 | the `edu-zeitlos` pack in this tree, built by `tools/learn/pack.py` and shipped by `release/` ([Packs](#packs)); one course on hardware | built; hardware next |
| L5 | the Ark's `edu` packs and tiers, built from the ark clone; the sdcard sets; `ask` indexing courses | |
| L6 | one chosen course, tested with real learners; then batches by the map, each through the accuracy steps | after L5 |

## The app

What bringing it up on the board taught, and the rules every change to
it follows, are in [learn_bringup.md](learn_bringup.md).

`sw/apps/learn/` is in the base set and in the dock (the wide row, and
Work on a side). What it does today:

- **Learners** (L on the home screen): pick or make one; the last one is remembered
  (`/data/learn/learn.cfg`). With a USB stick in, learners on it
  (`/usb/learn/NAME`) are listed first.
- **Home**: continue where you were, review due, your courses, the
  catalogue. **Catalogue** (F3 finds): courses from `/opt/ark/edu` and
  `/usb/edu`, by area and level, found a course at a time so the window
  never stalls. **Course**: its parts, units and lessons, each with a
  mastery disc.
- **Lessons**: words, headings, lists, tables, formulas typeset, code,
  `example` (worked, then faded), `note` and `warn` boxes, `sources`,
  `say` (speech; F6 reads the step aloud), `try` (an Open button starts the app),
  links (to a lesson, a course, a page of the docs, the web).
  Questions: choice, number, fraction and text, two tries with the
  author's feedback for each wrong answer, up to three hints (F5, which works while an answer is being typed), F8 to skip a question the learner does not care about (it counts as not right and does not come back in review), then the
  answer shown. A long step scrolls; Space shows the rest first.
- **Figures** (L3, [learn_figures.md](learn_figures.md)): Scheme in a
  sandbox, drawn beside the words; sliders, toggles, choices and points
  to drag; moving figures (P pauses); a library of number lines,
  fraction bars, graphs, clocks and more; black and white with patterns,
  and in colour on a board with the colour window when a figure asks
  for it (`(colour)`).
- **Review** (L3, [below](#review)): questions come back, spaced out;
  R on the home screen, and a warm-up before a lesson.
- **Progress**: one line a lesson in `progress.txt`, written as it
  happens, so a lesson resumes at its step; the done screen gives the
  score and names the next lesson.
- **Memory**: an arena of 4 MB, 3, 2 or 1.5 (whichever it gets); about
  500 KB of code and 1.2 MB of fixed buffers, of which Scheme's heap is
  480 KB and the figure's display list 75 KB. About 3.3 MB in all.
- **Trying a change on a board**: `make app-tftp APP=learn`, then
  `tget <host> release/dev/learn /apps/learn` on the board (README, "Core apps
  in flash"). A changed course: `tools/learn/pack.py build
  tools/learn/packs/edu-zeitlos.cfg`, `release/zrelease tftp`, and
  `update` on the board, which writes only the course's changed files.
- **Speed**: drawn straight into the window by the blitter and the
  line rasterizer, as the rest of the system draws; typing, a figure's
  controls and scrolling draw only what changed, the step's layout is
  kept, and a step's words show before its figure
  ([learn_figures.md](learn_figures.md#speed)).

Not yet (L4): order, match and point questions ("needs a newer
learn"), pictures, `try` steps that see what was done in the app (today
they open it, and a question checks it), the screen face, the
parent and teacher view.

The whole app runs on the build machine too: `make -C sw/apps/learn
test` runs the format, check, quiz and store tests, `learncheck` over
the shipped course, and a scripted session that writes every screen to
`tests/out` ([learn_ui.md](learn_ui.md#as-built-l2) says what changed from the design).
The first course on the card is **Using learn** (`zeit-learn`, four
lessons), which teaches the app itself, figures included.

### Review

Every question a learner answers is an item in five **Leitner boxes**
(`lreview.c`):

- **Right the first time, without a hint**: the question goes up a box,
  and comes back after 1, 3, 7, 16 or 35 days, by box. A new question
  answered right starts in box 2.
- **Missed, or answered after a hint**: back to box 1, and tomorrow.

The record is `review.txt` in the learner's folder: one line an answer,
added to the end, as `progress.txt` is (a line cut off by a power cut
is not an answer):

```
20387 r zeit-learn/start/questions#q1a2b3c4 1 01-start/02-questions.md
```

Each line holds the day, then `r` or `w` (right first time, or not, in
a lesson; `R` or `W` in a review), the question (its lesson's id and a
key: the question's `(id ...)`, or a hash of what it asks), its step,
and its lesson's file. The boxes are worked out from the lines when
the learner is opened. Without a clock, each time learn opens counts as
a day (a `D` line).

- **R** on the home screen (shown when something is due) reviews up to
  20 questions:
  - the longest-waiting first;
  - **courses taken in turn**, which interleaves them;
  - each question alone, with its figure but not the teaching around
    it, and its choices in another order.
- **Opening a lesson** whose course has questions due starts with a
  **warm-up** of up to three. Esc skips it.
- A question whose lesson changed so that it cannot be found any more
  (its words changed) is skipped; it starts again the next time it is
  answered in a lesson.

## Open questions

- **Japanese glyphs**: a vector kana face, and which kanji (and whose
  stroke data) for `lang-ja-1` and `lang-ja-2` (see
  [learn_ui.md](learn_ui.md#findings)).
- **Recorded speech** for the language courses and early phonics: who
  records, under CC0.
- **Memory**: learn with figures needs about 3.3 MB; it does not fit a
  1 MB board. A smaller build (a smaller arena, heap and list) is
  possible if a 2 MB board should run it.
- **Speed on the board**: how many evaluations a moving figure can
  afford at a dozen frames a second, measured on each board
  ([learn_figures.md](learn_figures.md#the-library)).
- **Regional supplements** for Life courses (money, law, first aid
  guidelines differ by country): which regions first.
