# learn: the screens

*Draft 1, 2026-10-26. The design of [learn](learn.md)'s screens,
written before it was built, and what changed as it was built.*

The screens as they are now are drawn on the build machine, at the
desktop's 640x480 and one bit a pixel, by learn's own code:
`make -C sw/apps/learn render` writes every screen of a scripted
session to `sw/apps/learn/tests/out/`, and `tools/learn/preview` every
step of a course ([learn_authoring.md](learn_authoring.md#writing-a-course-step-by-step)).
The design sketches that were here first are not kept in the tree.

## What every screen follows

- **One thing at a time.** A step teaches one idea; a check asks one
  question. Nothing else competes for the screen.
- **One key moves on.** Space or Enter is always "next" or "check";
  Backspace goes back; the digits choose an answer; Esc goes up. The
  footer names the keys that do something on this screen, and nothing
  else.
- **Any one input is enough.** Keyboard alone, mouse alone (every key
  in the footer is also a button), gamepad alone (A, B, the d-pad), or
  speech and the keyboard without the screen: F6 reads any step aloud.
- **Text you can read.** Body text is a 9x15 bitmap at about 30
  characters a line in the text column, never smaller than 7x13;
  headings and formulas are Hershey strokes at any size.
- **The figure pane is the same size everywhere**: 312x234 on the
  desktop face, inside the 320x240 the colour window can show, and the
  whole screen on the screen face. A figure is written once.
- **Honest about where things come from**: authors, review and sources
  are one key or one line away, on the course and on the step.

## The home screen

Two things matter on opening: what to do next, and what is due for
review. They are the two boxes at the top; Enter continues, R reviews.
Below, the learner's courses with their mastery (the quartered discs)
and progress, and the way into everything else.

## The catalogue

Areas on the left, courses in the middle, the chosen course on the
right: what it is, how long, who wrote it, whether and when it was
reviewed, what it is based on -- and for class B courses, its safety
note ([learn_format.md](learn_format.md#review-classes)).
**Test out** starts a placement check, so nobody has to sit through
what they know.

## A course

A part of a course (here grade 3 math's fractions) as its units, each
with its mastery and its standard codes, and the chosen unit's lessons.
The bottom lines are the course's record: authors, review, alignment,
version, errata fixed, and M to report a mistake.

## A teaching step

Words on the left, the figure on the right, the formula typeset. The
step dots in the header say where the learner is in the lesson. The
figure is drawn by the lesson (the `figure` block in
[learn.md](learn.md#what-is-proposed-markdown-for-words-s-expressions-for-everything-else)),
not a picture: it is a few lines of Scheme, it prints at any size, and
it can move.

On a board with the colour window, the same step, with the figure in
colour and the text still one bit:

## A check, and its feedback

The learner chose 2/5, the common mistake: a bigger number at the
bottom looks like a bigger fraction. The feedback answers that
misconception, the figure shows it, and "Try another" gives a new
question of the same kind rather than the same one again. The question
goes into review.

## An explorable figure

High school physics: the pendulum moves, and the slider changes its
length; the period is computed from the formula beside it (0.50 m gives
1.42 s) and marked on the curve of period against length. The step
ends with a prediction to test -- four times the length, twice the
period -- which is retrieval and experiment at once.

## The youngest learners: the screen face

Kindergarten counting on the 320x240 screen face, full screen and
pixel-doubled, in colour: large type, six big answers for a mouse or a
gamepad, and the speaker to hear the question again (the speech service
reads it when the step opens). The dots at the top right are the
lesson's steps.

The same face in its window on the desktop, with the colour titlebar
icon (F10 switches faces, F11 goes full screen):

## Review

Review is questions, spaced: a card answered right moves up a box and
comes back later; a card missed goes back to box 1. The five boxes and
their intervals are shown, so the system is never a mystery. Here a
hiragana card, typed in romaji, with a word that uses it.

## A language exercise

German 1: a typed answer checked exactly (umlauts matter, and the
footnote says how to type them with the `us-intl` layout), the
sentence's meaning, and the paradigm the question practises. "Hear it"
plays a recording, since the speech service speaks English only.

## A scenario, with a safety note

Wilderness survival: a scenario, a choice, the reasoning, a `warn` box
(drawn heavier, never skipped unread) and the source of the advice. The
right answer is the first action of the US Army survival manual's
SURVIVAL acronym: size up the situation, and do not hurry.

## Progress

For a parent, a teacher or the learner: time, mastery and what needs
work, per learner and course, and what went wrong this week in plain
words. P prints it. The note at the bottom is the rule: no streaks, no
reminders, nothing that asks for attention.

## Findings

Drawing these turned up things the design has to settle:

- **Japanese needs a better face.** The 12x12 bitmap font is right for
  Japanese text on the desktop, but enlarged for a flashcard (the か
  above) it is coarse, and stroke order cannot be taught from it at all.
  A vector kana face is needed, and stroke data for the course's kanji;
  KanjiVG's is CC BY-SA, so either a separately licensed pack or strokes
  drawn for the course. See [learn.md](learn.md#open-questions).
- **The colour figure pane needs the runtime's colour window for a
  rectangle inside a desktop window**, not only for a screen app's
  whole window ([learn.md](learn.md#two-faces)).
- **Bitmap text for reading, strokes for display.** At 640x480 and one
  bit, the misc-fixed faces are much easier to read in paragraphs than
  Hershey strokes at the same size; Hershey is right from about 20 px
  up (headings, formulas, numbers on a check). The screens use that
  split throughout.
- **Typeset fractions want their own layout rule.** A stacked fraction
  in running text needs more line spacing than a line of 9x15; a
  formula line (`$$`) gets its own line height.
- **Patterns carry the one-bit figures.** Two dither densities (the
  orange and blue bars, in colour) are enough to tell two fractions
  apart on one bit; a figure library's roles need a pattern each, and
  no more than three or four in one figure.

## As built (L2)

The same screens from the app itself, drawn by its scripted test
(`make -C sw/apps/learn render`, every picture in `tests/out`), with
the course that ships, **Using learn**:

What changed on the way from the design:

- **Titles, not ids**, everywhere a lesson is named (home, done); a
  title too long for its column ends in "...".
- **The done screen names the next lesson**, not its unit.
- **Esc goes back where you came from** (a course opened from the
  catalogue returns to the catalogue, not home).
- **Headings got more room below them** than the design gave; at one
  bit a heading crowding its paragraph reads as part of it.
- **A long step shows the rest before going on**: Space scrolls, then
  continues, so nothing is skipped by pressing on.

## As built (L3)

Figures, review and the warm-up, from the same scripted test:

What changed from the design:

- **Controls are under the figure**, not inside it. Learn draws them
  the same way in every figure (label, track, value), so they are
  reached the same way by keys and the mouse and need no space in the
  figure's 320 x 240. The mockup's pendulum had its slider in the
  picture.
- **The review card shows only when something is due.** With nothing
  due, Continue takes the whole width.
- **A review question stands alone**: what it asks, and the step's
  figure if there is one, but not the teaching words. Recalling is the
  point.
- **Lines on one bit are dashed by role.** Two plotted lines told apart
  only by colour would look the same on one bit.
