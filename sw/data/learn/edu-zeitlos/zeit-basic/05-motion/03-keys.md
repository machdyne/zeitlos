# Reading the keys

```lesson
(objective "Read the keys while a program runs with KEY, and steer something with the arrow keys.")
(minutes 20)
(keywords "key code")
```

In this lesson a running program starts listening to the keyboard.

`INPUT` stops the program until Enter. A game cannot stop: it must
keep moving and still notice keys. `KEY` does that. It is a function
that gives the next key pressed while the program runs, or 0 if no key
is waiting:

```basic
10 K = KEY
20 IF K THEN PRINT K
30 GOTO 10
```

`IF K` holds when K is not 0, so this prints a number for each key you
press, and nothing in between.

```check
(choice
  (ask "A program runs K = KEY when no key has been pressed. What is K?")
  (right "0")
  (wrong "The program waits until a key is pressed"
    "That is what INPUT does. KEY never waits: with no key waiting it gives 0 straight away.")
  (wrong "-1"
    "-1 is BASIC's true. KEY gives 0 when there is no key.")
  (hint "KEY never stops the program."))
```

---

## Key codes

The number for each key is its **key code**: for a character, the
number the computer stores it as. Some codes to know:

| Key | Code |
|---|---|
| Up, Down, Left, Right | 128, 129, 130, 131 |
| Enter | 13 |
| Space | 32 |
| Backspace | 8 |
| A (a capital) | 65 |

Esc is not a key code: it still stops the program. Keys pressed while
the program is busy wait in turn, up to 32 of them, and `KEY` gives
them oldest first.

```sources
(source "docs/basic_app.md, Beyond BASIC 1 (KEY)")
```

```try
(try (app "basic")
  (task "In BASIC, type NEW, then the three-line program above, then RUN. Press some keys, and try a with and without Shift. Press Esc to stop, and come back.")
  (check (choice
    (ask "Did a small a and a capital A give the same code?")
    (right "No: they are different characters, with different codes")
    (wrong "Yes: it is the same key"
      "KEY gives the code of the character typed, and a and A are different characters. Look again: A is 65.")
    (hint "Shift changes the character the key types."))))
```

---

## Steering

This program draws a line wherever you steer it with the arrow keys:

```basic
10 CLS: COLOR 1: X = 160: Y = 120
20 PLOT X, Y
30 K = KEY
40 IF K = 128 THEN Y = Y - 1
50 IF K = 129 THEN Y = Y + 1
60 IF K = 130 THEN X = X - 1
70 IF K = 131 THEN X = X + 1
80 GOTO 20
```

```check
(choice
  (ask "Which arrow key makes Y smaller, and so moves the point up?")
  (right "Up, code 128")
  (wrong "Down, code 129"
    "Y grows downwards, so Down makes Y bigger. Up, 128, makes it smaller: line 40.")
  (hint "Look at line 40. Remember that y counts down from the top."))
```

---

```check
(choice
  (ask "You hold no key, and the program above keeps running. What does it do?")
  (right "It plots the same point again and again")
  (wrong "It stops and waits for a key"
    "KEY does not wait: it gives 0, no IF matches, and the loop goes round again.")
  (wrong "The point keeps moving in the last direction"
    "Each key press moves the point once. With no key, K is 0 and nothing changes.")
  (hint "What is K when no key is waiting?"))
```

---

## Keeping on the screen

To stop the point leaving the screen, test the edge as well as the key
with `AND`:

```basic
60 IF K = 130 AND X > 0 THEN X = X - 1
```

```check
(text
  (ask "Finish line 70 so the point cannot go past the right edge: 70 IF K = 131 AND X < ... THEN X = X + 1")
  (accept "319")
  (wrong-if "320" "With X < 320, X could become 320, just off the screen. The last pixel is 319.")
  (wrong-if "318" "Then X could reach only 318, one short of the last pixel. X < 319 lets it reach 319.")
  (hint "The last pixel across is 319. X must be below it before it can go up by 1."))
```

So: `KEY` gives the next key pressed, or 0, without stopping the
program, and its key codes let a program steer.
