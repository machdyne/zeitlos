# Pictures and diagrams

```lesson
(objective "Put a picture on a card, and draw a flowchart whose arrows follow its shapes.")
(minutes 20)
(keywords "flowchart" "connector (everyday word)" "group (in slide)")
```

The image tool, **i** (or the command `image`), asks for a picture file
and puts it on the card: a photo, or a painting from `draw`.

The app never stops to wait for a picture. Until it has loaded, the
picture is an empty box with a bar that fills up, and the status line says
`Loading NAME 45%  (Esc stops)`.

- **Esc** stops the loading: the picture shows a light cross.
- **Enter** on it, selected, loads it again.
- A file that cannot be read shows a dark cross.

```sources
(source "slide (docs/slide_app.md), Pictures load in the background")
```

```check
(choice
  (ask "A large photo is loading on a card and you need slide now. What do you do?")
  (right "Press Esc; later, select the picture and press Enter")
  (wrong "Wait: slide cannot be used until it has loaded" "slide goes on working while a picture loads. You can also stop it with Esc.")
  (wrong "Close slide and open it again" "There is no need. Esc stops the loading, and Enter on the picture starts it again.")
  (hint "The status line says which key stops it."))
```

---

## A flowchart

A **flowchart** is a drawing of steps and choices joined by arrows.
slide has its shapes as commands, each with words in the middle:

- `pill`: a rounded bar, for the start or the end;
- `diamond`: a choice, such as `diamond Raining?`;
- `slant`: a slanted box, for something in or out;
- `document` and `cylinder`: a paper, and a store.

Steps can be plain rectangles. Enter or a double-click on any shape
types into it.

```figure
(define (pill x y w h s)
  (rect (+ x 8) y (- w 16) h 'paper)
  (frame x y w h 'ink 1.5)
  (text (+ x (/ w 2)) (+ y (/ h 2) 4) s 'centre 'small))
(pill 120 10 80 26 "Start")
(poly '(160 56 230 86 160 116 90 86) 'paper)
(path '(160 56 230 86 160 116 90 86 160 56) 'ink 1.5)
(text 160 90 "Raining?" 'centre 'small)
(mk-box 30 150 90 30 "Take a coat")
(mk-box 200 150 90 30 "Go out")
(arrow 160 36 160 56 'ink 1.5)
(path '(90 86 75 86) 'ink 1.5)
(arrow 75 86 75 150 'ink 1.5)
(path '(230 86 245 86) 'ink 1.5)
(arrow 245 86 245 150 'ink 1.5)
(text 70 80 "yes" 'right 'small)
(text 250 80 "no" 'left 'small)
(caption "A flowchart: a start, a choice, two steps")
```

```check
(choice
  (ask "In a flowchart, which shape is for a question with a yes or a no?")
  (right "A diamond")
  (wrong "A pill" "A pill, a rounded bar, marks where the flowchart starts or ends.")
  (wrong "A cylinder" "A cylinder is a store, a place where things are kept.")
  (hint "Its command in slide is diamond."))
```

---

## Connectors

Select two shapes (a click, then Shift+click) and type the command
`connect`. slide draws an arrow from the first to the second: a
**connector**. Move or resize either shape, and the connector follows.

`connect elbow` goes across, along and across, as a flowchart's lines
often do. Drawing a line with the line or arrow tool from one shape to
another connects them too.

```sources
(source "slide (docs/slide_app.md), Diagrams")
```

```check
(choice
  (ask "Two shapes are joined by a connector. You drag the second shape down the card. What does the arrow do?")
  (right "It follows the shape, still joining the two")
  (wrong "It stays where it was" "A plain line would. A connector knows its two shapes and is drawn between them again.")
  (wrong "It is deleted" "Moving a shape does not delete anything; the connector is drawn to where the shape is now.")
  (hint "What makes a connector different from a line?"))
```

---

## Groups

Select several things and press **Ctrl+G**: they become a **group**,
which a click selects whole and which moves, copies and deletes as one.
**Ctrl+Shift+G** parts it again.

```check
(choice
  (ask "A title and a picture are a group. You click the picture and press Delete. What is deleted?")
  (right "Both: a group deletes as one")
  (wrong "Only the picture" "A click on any part of a group selects all of it. To delete one part, part the group first with Ctrl+Shift+G.")
  (wrong "Nothing: a group cannot be deleted" "A group can be deleted like anything else, all of it at once.")
  (hint "What does a click on one part of a group select?"))
```

---

```try
(try (app "slide")
  (task "Open slide from the dock. Type /pill Start and Enter, then /diamond Ready? and Enter. Drag them apart with v. Click the pill, Shift+click the diamond, and type /connect. Now drag the diamond somewhere else. Then come back.")
  (check (choice
    (ask "Where is the arrow now?")
    (right "Still from the pill to the diamond")
    (wrong "Where the diamond used to be" "A connector follows its shapes when they move.")
    (wrong "It has gone" "Moving a shape does not delete its connector."))))
```

So: i places a picture, which loads while you work; flowchart shapes
are commands; `connect` joins two shapes with an arrow that follows
them.
