# learn: writing courses

*Draft 1, 2026-10-26. For people and models writing courses in
[the format](learn_format.md), against the Ark's curriculum map ([`edu/curriculum.md`](https://github.com/machdyne/ark/blob/main/edu/curriculum.md)) or, for Zeitlos's own courses, [learn.md](learn.md#packs),
for [learn](learn.md). The rules here are the ones a reviewer checks;
the ones a program can check, `learncheck` checks.*

## The three rules

1. **True.** Every claim is right, at the level it is stated, for every
   English-speaking learner. A simplification is allowed only if what
   it leaves out does not make it false ("plants make food from light",
   not "plants eat sunlight"). When in doubt, leave it out.
2. **Taught, not told.** One idea a step, checked before the next. The
   learner does something in every step or two.
3. **Short.** A step fits on the screen beside its figure: 150 words at
   most, usually far fewer. A lesson is 15-30 minutes.

## Rules from the first courses

The first two courses, *Using learn* and *Timeless Computing*, were
taken through on a board (rounds r6-r20, [learn_bringup.md](learn_bringup.md)).
What went wrong in their content is now a rule for every course:

1. **No word before it is taught.** The first lesson of Timeless
   Computing said a wire carries "a voltage" to learners who had never
   met the word. Every term of art is either taught in an earlier lesson
   of the course, taught in a course this one `needs`, or defined, in
   bold, in the step that first uses it. Each lesson lists the terms it
   teaches in its `keywords`; a term that is also an everyday word gets
   a note in brackets (`"word (of bits)"`) and is then checked only
   inside its course. `tools/learn/terms.py FOLDER` checks the order
   across the whole pack; the fact-check (step 4) lists any term used
   without being taught, which no program can know.
2. **Say what the course assumes.** `needs` in `course.cfg` for every
   course whose words it uses, and the course's `index.md` says in a
   sentence what a learner should already know.
3. **A figure models what it teaches, truthfully.** A clock keeps time
   and has a second hand; a counter counts in the base the step is
   about; a circuit's lamp is lit only when the circuit would light it.
   A figure that moves wrongly teaches the wrong thing, however pretty.
4. **Black and white first.** Figures read in one colour with patterns
   and labels; `(colour)` only where colour is the thing being taught
   (the RGB of a pixel). Never "the red wire": label it.
5. **Facts about Zeitlos come from the tree, not from memory.** A key,
   a command, an app's name, a path, a menu: each is checked against
   `docs/` and, where the docs are thin, the app's source. A lesson
   that teaches a key the app does not have is worse than no lesson.
   Link the page (`[the dock](docs:dock)`) so the learner can read more.
6. **Teach only what learn can do today.** No order, match or point
   questions yet; no pictures in edu-zeitlos. A `try` step shows an Open
   button that starts the app; write its task so it also works for a
   learner who opens the app from the dock instead, and says how to come
   back to learn.
7. **Typed answers accept every right way of writing them.** `0110` and
   `110`; `F0` and `0xF0`; a command with and without a trailing space
   is the same command. Ask for one short thing at a time.
8. **Keys in the words are the keys on the board.** In learn: F5 a hint
   (it works while typing an answer), F6 read aloud, F7 report a
   mistake, F8 skip, Space next, Backspace back, Esc leave. The function keys
   mean the same in every app ([keys.md](keys.md)).
9. **Look at every step before the board does.** `learncheck` with no
   errors and no warnings, `terms.py` with no warnings, and
   `tools/learn/preview` drawing every step and answering every
   question: then look at the pictures. Words that run past a figure,
   a figure that does not show what the step says, a question learn
   does not take its own answer for: each was a round on the board.
10. **Figures are cheap.** A frame is drawn for every slider move on a
    50 MHz processor: no loops over hundreds of points per frame, no
    recomputing what does not change. `learncheck` warns at 60 ms on the
    build machine; aim far below it.

## Writing a course, step by step

Each step produces a file someone can review before the next begins.
A model doing this works in the same order, one step per request.

1. **The outline**, from the course's row in the map: its parts and
   units, each unit's objective in one sentence and its standards, and
   for each unit its lessons' titles and objectives. Reviewed before
   any lesson is written: a wrong outline is the most expensive mistake.
2. **The lessons**, one file each, in the outline's order.
3. **The checks pass**: `learncheck` on the course; fix every error and
   read every warning; `tools/learn/terms.py` on the pack. Then **look at it**: `make -C tools/learn/preview
   run COURSE=id` draws every step as learn draws it, a PNG a step in
   `tools/learn/out/preview/id/`, and answers every question rightly
   through the keys, as a learner would; a question learn does not take
   its own answer for is a mistake. Look at every picture: words that
   run long, a figure that does not show what the step says.
4. **The fact-check**: every claim in every lesson checked against a
   source, by someone (or a separate model run) that did not write it.
   Disagreements go to a person.
5. **The review**: a person reads the course as a learner would, then
   against the checklist below. Class B courses go to a qualified
   reviewer ([learn_format.md](learn_format.md#review-classes)).
   The reviewer's line goes into `course.cfg`.

## A lesson's shape

The format does not force this shape, but every lesson should have it:

| Part | What | In the file |
|---|---|---|
| warm-up | learn adds 2-4 review questions itself | nothing |
| the point | the objective, as the learner would say it | the first step's first sentence |
| teach | the idea in small steps, each with a figure where one helps | prose + `figure` |
| check | a question after every step or two | `check` |
| worked example | a problem solved step by step, then one with the last step to do | `example` |
| practice | three to six questions, mixed in difficulty | `check` |
| close | one sentence: what was learned | the last step |

### Questions

- **Ask for retrieval, not recognition, where you can.** A typed
  answer (`number`, `fraction`, `text`) asks more of memory than a
  choice; use choices where the answer is an idea, not a word.
- **Wrong answers are real mistakes.** Every distractor is something a
  learner at that level actually thinks: the bigger denominator is the
  bigger fraction; heavier things fall faster; a spider is an insect.
  A distractor nobody would choose teaches nothing.
- **Feedback answers the mistake.** Not "Wrong, it is 2/3", but why
  the reasoning that leads to 2/5 fails. Two or three sentences.
- **Hints are a ladder.** The first points at what to look at; the
  second narrows it; the third is nearly the answer.
- **Put `(true ...)` on every arithmetic answer, and `(from ...)` on
  every computed number**, so the checker can prove them.
- Never ask about the wording of the lesson ("What did the lesson call
  ...?"); ask about the idea.

### Words

| Level | Sentences | Words |
|---|---|---|
| K-2 | 5-10 words; one idea each | the words a child says; new words taught, said aloud (`say`) |
| 3-5 | up to 15 words | a new term once a step, defined when it first appears |
| 6-8 | up to 20 words | subject terms, defined |
| high school | up to 25 words | the subject's terms |
| college | as clear as the subject allows | the field's terms; formulas typeset |
| life, Zeitlos | plain adult English | no jargon the reader does not need |

- Write to the learner: "you", present tense, active voice.
- Define a term the first time, in the sentence that uses it.
- Numbers as digits ("3 pieces"), except where the lesson is about
  number words.
- **US spelling and units for school courses** (they follow US
  standards), with metric alongside where science uses it; Life
  courses give both units.
- No idioms or cultural references a learner elsewhere would not know;
  no jokes at anyone's expense.
- Not a single character the fonts cannot draw: Latin-9 (accented
  Latin letters, €) plus the curly quotes and dashes, which are drawn as
  their plain look-alikes. Greek letters, arrows and other symbols go
  in formulas (`$theta$`), not in text. `learncheck` warns.

### Figures

- A figure shows the idea, not decoration. If it does not help the
  question that follows, leave it out.
- Use the figure library's procedures (fraction bars, number lines,
  axes and plots, clocks...) rather than drawing shapes by hand.
- Every figure must read in black and white: colours are by role, and
  each role has a pattern. Do not say "the red bar"; say "the shaded
  bar" or label it.
- A picture (`![...](img/...)`) only where drawing would be dishonest
  -- anatomy, a map, a photograph of a plant or a work of art -- and
  only from a public-domain or CC0 source, with its line in media.cfg.

## Accuracy

- **Every claim beyond common knowledge at the course's level has a
  source** in a `sources` block on its step: a standard reference, a
  government publication, a primary source. Prefer sources in the Ark
  library, which a learner can open on the machine.
- **Never invent a source**, a quotation, a statistic, a date or a
  name. A model that is not sure of a source writes none and marks the
  claim for the fact-check (`(note "CHECK: ...")` in a draft, removed
  before review).
- **Time-sensitive facts** (laws, guidelines, prices, populations,
  records) are dated in the text ("as of 2026") and the course is class
  C. Prefer timeless facts: the reason a law exists outlives the law.
- **Contested questions** (class D) give the strongest form of each
  main position, attributed, and do not choose for the learner.
- **Safety** (class B): say what can go wrong and what to do instead,
  in a `warn` block, before the step that needs it. Never teach a skill
  whose mistakes kill without saying that it must be practised under
  supervision; never teach identifying edible plants or mushrooms from
  pictures; first-aid steps follow current published guidance, dated,
  and say when to get help.
- **Do not reproduce copyrighted text.** Literature is public domain;
  paraphrase everything else in your own words.

## The reviewer's checklist

- [ ] Every claim is true, at the level stated. Spot-check five
      against their sources; check every number.
- [ ] Nothing the course needs is missing from the outline; nothing in
      it belongs in another course.
- [ ] Each step is one idea, followed soon by a check.
- [ ] Each distractor is a real mistake; each feedback answers it.
- [ ] The words fit the level; terms are defined where they appear.
- [ ] Figures help, and read in black and white.
- [ ] Safety notes are where they are needed (class B: by a qualified
      reviewer).
- [ ] Sources are real and say what the lesson says they say.
- [ ] `learncheck --release` passes.

## Prompts

Prompts for each step of writing a course with a model. Each is sent
with this page, [learn_format.md](learn_format.md) and the course's row
of the map; a lesson prompt also carries the outline and the lessons
before it in the unit. They are starting points to be refined on the
first course.

**Outline.**

> Write the outline of the course `ID` described in the attached map
> row. For each part and unit: a title, a one-sentence objective, its
> standard codes; for each unit, its lessons as numbered file names
> with a one-sentence objective each. Lessons are 15-30 minutes; units
> are 3-8 lessons. Follow the order of the standards where they give
> one. Write it as the folder tree and `index.md` titles, plus a
> `course.cfg`. Do not write lessons yet.

**A lesson.**

> Write the lesson `FILE` of the outline, in the learn format. Follow
> the lesson shape, the question rules and the word rules for level
> `LEVEL`. Teach exactly its objective. Every arithmetic answer has a
> `(true ...)`, every computed number a `(from ...)`. Every wrong
> answer is a real misconception with its feedback. Cite a source for
> every claim beyond common knowledge at this level; if you are not
> sure of a source, write `(note "CHECK: ...")` instead of inventing
> one. Use figure library procedures, not raw drawing.

**The fact-check.** (A separate run, without the conversation that
wrote the lesson.)

> Here is a lesson. List every claim of fact it makes, one per line,
> each with: the claim, whether it is true at the level stated (true /
> false / misleading / cannot tell), and why. Check every number and
> every source it cites. Do not rewrite the lesson.

**Fixing.**

> Here is a lesson and its fact-check. Change only what the fact-check
> found false or misleading, and what `learncheck` reports. Keep
> everything else as it is.

### What it costs

A rough guide for planning, to be replaced by what the first course
actually takes: a lesson is about 5-10 KB, which is some 2,000-3,500
tokens of output. With its context (this guide, the format, the
outline, the unit so far) and the fact-check and fixing passes, a lesson
is likely 20,000-40,000 tokens in all, and an outline a few tens of
thousands. The Ark's map is about 11,600 lessons.
Measure the first course (`learncheck` prints its lessons, steps and
words) before scheduling batches.
