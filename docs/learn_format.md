# learn: the course format

*Format version 1. The code that reads it is `sw/apps/learn/lformat.c`
(the model), `lsx.c` (data blocks), `lmath.c` (formulas) and
`lcheck.c` (the checks); the tool is `tools/learn/learncheck`.
[learn.md](learn.md) is the design this serves, and
[learn_authoring.md](learn_authoring.md) how to write in it.*

A course is a folder of plain files that a person or a model can
write, a reviewer can read, and `learncheck` can verify. Words are
Markdown. Everything that is not words -- questions, flashcards, worked
examples, safety notes, sources -- is a fenced block of S-expressions,
read as data and never run. Figures are fenced blocks of Scheme, run by
learn in a sandbox. Nothing is positioned by coordinates: learn lays a
lesson out for the window, the 320x240 screen or the page.

## A course

```
math-3/                         the course's id is its folder's name
  course.cfg                    what the course is
  media.cfg                     where each picture and sound comes from
  index.md                      optional: the course's # title and an introduction
  04-fractions/                 a part (it holds units)
    index.md                    optional: the part's # title
    05-comparing/               a unit (it holds lessons)
      index.md                  optional: the unit's # title
      01-same-numerator.md      a lesson
      02-same-denominator.md
  img/                          pictures, anywhere in the course
  lib/                          optional: Scheme for the course's figures (*.scm)
```

- A **numbered folder** (`05-comparing`) at the top of the course is a
  **part** if it holds numbered folders and a **unit** if it holds
  numbered lesson files. A course has parts or units at its top, not
  both. Files and folders without a number are not part of the order
  (`index.md`, `img/`, `notes/`).
- The number sets the order and nothing else: it is not part of any
  id. A lesson's **id** is its path without numbers and without `.md`:
  `math-3/fractions/comparing/same-denominator`. Progress is recorded
  against ids, so lessons can be renumbered and reordered without
  losing anyone's progress; renaming the words of a file changes its id.
- A title comes from the first `# ` line of the folder's `index.md`, or
  of the lesson; failing that, from the name (`05-comparing` is
  "Comparing").

### course.cfg

`key = value` lines, `#` comments, as in `zeitlos.cfg`; but these are
learn's own keys, and a key that can repeat (`author`) makes a list.

| Key | | |
|---|---|---|
| `id` | required | the folder's name: lower case letters, digits, `-` (the Ark's curriculum map, `edu/curriculum.md` in [the Ark](https://github.com/machdyne/ark)) |
| `title` | required | as the catalogue shows it |
| `level` | required | `K`, `1`..`12`, `ms`, `hs`, `col`, `life`, `zeit`, `lang` |
| `area` | required | `school`, `college`, `life`, `lang`, `zeit` |
| `class` | required | review classes, letters from `A B C D` ([below](#review-classes)) |
| `version` | required | 1, then one more for each release of the course |
| `format` | | the format version it is written for: `1` |
| `author` | required, repeats | a person, an organisation or a model: `Claude (Anthropic model)` |
| `reviewer` | repeats | `name; qualification; yyyy-mm-dd` |
| `subject` | | `math`, `reading`, `science`, `survival`... |
| `ages` | | `12+` |
| `lang` | | `en` (`ja` turns on the Japanese font for the checks) |
| `license` | | `CC0-1.0` for the edu pack |
| `summary` | | one line for the catalogue |
| `standards` | | what it is aligned with: `CCSS grade 3` |
| `needs` | repeats | a course to have done first: `math-2` |
| `parent-opt-in` | | `yes`: hidden from a learner unless a parent turns it on |

### Review classes

How a course is checked before it is marked
reviewed (details in [learn.md](learn.md#accuracy), and how to write each in [learn_authoring.md](learn_authoring.md#accuracy)):

| Class | Kind of content | Before it is marked reviewed |
|---|---|---|
| **A** | Settled knowledge, low harm if misread: arithmetic, grammar, most science | mechanical checks, an independent fact-check of every claim, a human review |
| **B** | Safety-critical: first aid, survival, health, food safety, electricity, chemistry done at home, money and law | as A, plus review by a qualified person (an EMT for first aid, a wilderness instructor for survival...). **Not shipped before that review.** |
| **C** | Time-sensitive: laws, tax rules, medical guidelines, statistics about the present | as A or B, every such claim dated ("as of 2026") and sourced, and re-reviewed on a schedule |
| **D** | Contested or value-laden: recent history, politics, ethics, religion in history | as A, with competing views presented fairly and sourced, reviewed by more than one reviewer |

A course can be several classes (`A B`). `learncheck --release` holds a
class B course to a qualified reviewer and a class D course to two.

### media.cfg

One line per picture or recording, its path in the course, then its
source and licence:

```
img/heart.png = Gray's Anatomy (1918), plate 494; public domain
img/moon.png = NASA, Apollo 11 AS11-44-6667; public domain (US government work)
```

A picture with no line here is an error: every picture says where it
came from.

## A lesson

````
# Same denominator

```lesson
(objective "Compare two fractions with the same denominator.")
(standards "3.NF.A.3d")
(minutes 20)
```

When two fractions have the same denominator, the whole is cut into
pieces of the same size. Then the fraction with more pieces is the
greater one.

$$ 3/4 > 2/4 $$

```figure
(frac-bar 0 4 3 'warm "3/4")
(frac-bar 1 4 2 'cool "2/4")
```

---

```check
(choice
  (ask "Which is greater?" (math "3/5") "or" (math "4/5"))
  (right "4/5 is greater" (true "4/5 > 3/5"))
  (wrong "3/5 is greater"
    "Both are fifths, so every piece is the same size. 4 pieces are more than 3.")
  (hint "Picture both bars. Which has more pieces shaded?"))
```
````

- The first line is the lesson's **`# title`**, once.
- The **`lesson` block** says what the lesson is: `(objective "...")`
  (required: one sentence, what the learner can do afterwards),
  `(standards "...")`, `(minutes N)`, `(needs "lesson-id")`,
  `(keywords "..." ...)`.
- **`---`** on a line of its own ends a step. A step is one idea, with
  what teaches it and what checks it; learn shows one step at a time.
- Words are **Markdown**:

| Written | Is |
|---|---|
| a paragraph | text, laid out to fit |
| `## Heading`, `### Heading` | headings inside a step |
| `- item`, `1. item` | lists (indent two spaces to nest) |
| `> words` | a quotation |
| `\| a \| b \|` | a table, one row a line (`\|---\|` rows are dropped) |
| `![what it shows](img/x.png)` | a picture from the course (and its line in media.cfg) |
| `$$ formula $$` | a formula on a line of its own |
| `**bold**`, `*italic*` or `_italic_`, `` `code` `` | as you would expect |
| `$formula$` | a formula in the text |
| `[words](target)` | a link: `lesson:unit/lesson` (in this course, or `lesson:course/...`), `course:id`, `docs:page` (a page of the Zeitlos docs), `https://...` (warned: it needs the network) |
| `\*`, `\$`, `` \` `` | the character itself |

- Any other fenced block (```` ```basic ````, ```` ```c ````) is **code**,
  shown as written in a fixed-width font.

### Formulas

One notation for typesetting and for checking: learn draws it, and
`learncheck` computes it. It reads correctly as plain text.

| Written | Means |
|---|---|
| `3/4` | a fraction (drawn stacked) |
| `2 1/2` | a mixed number |
| `x^2`, `x_1` | power, subscript |
| `2x`, `2 pi r`, `xy` | multiplication, implied: a run of letters is one letter each, except a known name |
| `*` `×` `·` | times; `÷` divided by (drawn inline) |
| `+` `-` `−` | plus, minus |
| `25%` | per cent (a quarter) |
| `sqrt(x)` `abs(x)` `sin` `cos` `tan` `ln` `log` `exp` | functions; `log` is base 10 |
| `pi`, `e` | the constants |
| `alpha` .. `omega`, `Gamma` .. `Omega` | Greek letters by name |
| `<` `>` `<=` `>=` `=` `!=` (or `≤ ≥ ≠`) | comparisons, chainable: `0 < x < 1` |
| `( )` | brackets, kept as written |

Computation is exact with fractions while the numbers fit (`2/4`
equals `1/2`, `0.1 + 0.2` equals `0.3`) and in floating point beyond.

## Data blocks

Each block holds forms `(name ...)`. Strings are `"..."` with `\"`,
`\\` and `\n`; numbers are written plainly; `;` starts a comment.

### check: a question

Every kind takes `(ask ...)` -- the question, strings and `(math
"...")` -- and up to three `(hint "...")`, a ladder from a nudge to the
worked answer. `(explain "...")` is shown after a right answer.
`(id "name")` gives the question a name of its own for review
(otherwise it is the lesson's id and its place).

**choice** -- pick one (or several, with `(several)`):

```
(choice
  (ask "Which is greater?" (math "2/3") "or" (math "2/5"))
  (right "2/3 is greater" (true "2/3 > 2/5"))
  (wrong "2/5 is greater"
    "Both have 2 pieces. Fifths are smaller than thirds, so 2/3 is greater.")
  (wrong "They are equal" "The pieces are different sizes, so they cannot be equal.")
  (hint "Which is bigger: a third of a pizza or a fifth?"))
```

Every wrong answer has its feedback: what to say to someone who chose
it -- the misconception that leads there, answered. Two to six answers;
learn shows them in a shuffled order unless `(keep-order)`.

**number** -- a typed number:

```
(number
  (ask "A pendulum 0.5 m long. Its period, in seconds?")
  (answer 1.42) (within 0.01) (unit "s")
  (from "2 pi sqrt(0.5/9.81)")
  (wrong-if 2.84 "That is two swings. The period is one: there and back once."))
```

`(from "formula")` is how the answer was worked out: `learncheck`
computes it and fails the lesson if `(answer)` is not that within
`(within)`. Give it wherever the answer is computed.

**fraction** -- a typed fraction: `(answer 3 4)`; `(equivalent)` to
accept `6/8`, `(simplest)` to require lowest terms; `(wrong-if (4 3)
"...")`.

**text** -- typed words: `(accept "bin")`, more than one allowed;
case is ignored unless `(case)`; `(accents strict)` (default) or
`(accents nearly)`, where a missing umlaut is "nearly -- check the
accents"; `(wrong-if "ist" "That is er, sie, es. For ich ...")`.

**order** -- put in order: `(items "first" "second" "third")`, written
in the right order; learn shuffles them.

**match** -- pair them up: `(pair "left" "right")`, two or more.

**point** -- click a place on the figure: `(target X Y R)` in the
figure's units.

### Claims: `(true ...)` and `(false ...)`

Anywhere in a data block, `(true "formula")` states that a comparison
holds and `(false "formula")` that it does not. They are not shown;
`learncheck` evaluates them and fails the lesson if one is wrong. Put
one on every answer whose rightness is arithmetic:

```
(right "4/5 is greater" (true "4/5 > 3/5"))
(wrong "They are equal" (false "4/5 = 3/5") "...")
```

### example: a worked example

```
(example
  (problem "Compare 3/8 and 5/8.")
  (step "Both are eighths: the pieces are the same size.")
  (step "5 pieces are more than 3.")
  (step (choice (ask "So which is greater?") (right "5/8")
          (wrong "3/8" "Count the pieces: 5 are more than 3.")))
  (answer "5/8 is greater"))
```

learn shows the steps one at a time. A step can be a check, so an
example fades: the next one asks for the last step, then the last two.

### cards: flashcards for review

```
(card "か" "ka" (note "as in かさ, kasa, umbrella"))
(card "der Hund" "the dog" (typed))
```

Front, back, an optional note. `(typed)`: the learner types the back
(checked as `text`); otherwise they say whether they knew it. Cards go
into the learner's review queue.

### try: do it in a real app

```
(try (app "calc") (task "Work out 37 × 24 with the calculator.")
  (check (number (ask "What did you get?") (answer 888) (from "37*24"))))
```

learn opens the app beside the lesson, then asks the check. The apps a
lesson can open: basic, repl, term, calc, sheet, text, read, draw, pix,
slide, bench, logic, files, chess, zcc, hex, view, web, keyboard,
settings, cal, clock, midi, track, play.

### say, note, warn, sources

```
(say "How many apples are there?")                   ; spoken when the step opens
(note "A coin and a ring are both circles.")          ; an aside
(warn "Cold and wet" "Wet clothes lose heat fast, even well above freezing.")
(source "FM 21-76, Survival (US Army, 1992), chapter 1")
```

A `warn` is drawn heavier than anything else and cannot be skipped
unread. A `source` names where the step's claims come from: title,
author or publisher, year, and the part.

## Figures

```` ```figure ```` blocks are Scheme ([scheme.md](scheme.md)) that
draw in a 320 x 240 unit space beside the step's words. Their colours go
by role (`'ink 'paper 'warm 'cool 'good 'bad 'accent 'sky 'earth 'plant
'grey`), each with a one-bit pattern too. They can have controls
(sliders, toggles, choices, points to drag) and time. A step's figure
blocks are joined into one figure.

A figure sees nothing else: no files, no network, no programs. A
course's own procedures go in `lib/*.scm` in its folder, read before
each of its figures. The vocabulary, the library (`frac-bar`,
`number-line`, `graph` and `plot`, `clock-face`, ...), how figures run
and how they are checked are in [learn_figures.md](learn_figures.md).

## Checks

`learncheck PATH...` checks courses (a course folder, or a folder of
them: then links and `needs` between them are checked too). It prints
`file:line: error: ...` and exits 1 on any error. `--release` adds the
release rules.

| | Errors | Warnings |
|---|---|---|
| course.cfg | a missing required key; an id that is not the folder's name; an unknown level, area or class; `needs` a course that does not exist; a reviewer line not `name; qualification; date` | an unknown key; no summary, lang or licence; not CC0 |
| structure | a lesson that does not parse (a block not closed, two titles); a course with no lessons | a unit with no lessons; a part with no units |
| lessons | no `# title`; a lesson block without an objective; a step over 250 words | no lesson block; a step over 150 words; three teaching steps without a check; no check at all; minutes outside 5-45; prose well above the course's grade (Flesch-Kincaid) |
| checks | not a kind of check; no `(ask)`; a choice without a right or a wrong answer, two right without `(several)`, an answer twice, a wrong answer without feedback; a `(from)` that does not give the `(answer)`; a claim that is false; `(wrong-if)` without feedback; text with nothing to accept | more than six answers or three hints |
| words | a formula that does not parse; a link to a lesson that does not exist; a picture missing or not in media.cfg | a character the fonts cannot draw (a Greek letter in text: write a formula); a link that needs the network |
| figures | brackets or strings not closed; a figure that fails when run: at its start, at a later time, or with a control at either end ([learn_figures.md](learn_figures.md#checks)) | drawing outside 320 x 240; a frame slow to draw |
| release | not reviewed; class B without a qualified reviewer; class D with fewer than two | |
| notes | class B: not to be released before a qualified review | |

## Limits

| | |
|---|---|
| a lesson file | 48 KB |
| steps in a lesson | 32 |
| blocks in a lesson | 160 |
| answers to a choice | 6 (warned above) |
| hints | 3 (warned above) |
| units in a course / lessons | 160 / 1,200 |
| `author`, `reviewer`, `needs` lines | 8 each |
