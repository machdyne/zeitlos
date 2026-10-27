# Waiting

```lesson
(objective "Make a program wait for a time with WAIT and SLEEP, and use waiting to time what it shows.")
(minutes 15)
(keywords "millisecond")
```

In this lesson your programs learn to take their time.

A computer runs a program as fast as it can. To slow it down, a program
waits:

- `SLEEP s` waits s seconds;
- `WAIT m` waits m **milliseconds**. A millisecond is a thousandth of a
  second, so `WAIT 1000` waits one second and `WAIT 250` a quarter of a
  second.

Esc stops a program while it waits, too.

```sources
(source "Machdyne BASIC 1 reference (sw/ext/basic/docs/basic1.md), section 5, SLEEP and WAIT")
```

```check
(number
  (ask "How many milliseconds does a program wait for half a second?")
  (answer 500) (from "1000 / 2")
  (wrong-if 50 "A second is 1000 milliseconds, so half of one is 500.")
  (hint "A second is 1000 milliseconds."))
```

---

## A countdown

```basic
10 FOR I = 10 TO 1 STEP -1
20 PRINT I
30 SLEEP 1
40 NEXT I
50 PRINT "LIFT OFF"
```

```check
(number
  (ask "About how many seconds after RUN does LIFT OFF appear?")
  (answer 10)
  (wrong-if 9 "The program sleeps after every number, 10 down to 1 as well: 10 sleeps of a second.")
  (hint "How many times does SLEEP 1 run?"))
```

---

## Whole seconds only

Numbers are whole, so `SLEEP` waits only whole seconds. For anything
shorter, or between whole seconds, use `WAIT`: one and a half seconds
is `WAIT 1500`.

```check
(choice
  (ask "Which line waits a tenth of a second?")
  (right "WAIT 100")
  (wrong "SLEEP 0.1"
    "This BASIC has whole numbers only, so 0.1 cannot be written. WAIT counts in milliseconds: a tenth of a second is 100.")
  (wrong "WAIT 10"
    "WAIT 10 is 10 milliseconds, a hundredth of a second.")
  (hint "A tenth of 1000 milliseconds."))
```

---

## Blinking

Waiting and inverting make a shape blink:

```basic
10 CLS: COLOR 2
20 BOX 140, 100, 180, 140, 1
30 WAIT 500
40 GOTO 20
```

Each pass inverts the box: on, off, on, off, half a second each. It
runs until you press Esc.

`COLOR 2` stays set after the program stops, even for the next `RUN`,
until a program sets `COLOR` again. So a drawing program that follows
should start with `COLOR 1`.

```try
(try (app "basic")
  (task "In BASIC, type NEW, then the blinking program, then RUN. Change line 30 to WAIT 100 and RUN again. Press Esc to stop it, and come back.")
  (check (choice
    (ask "What did WAIT 100 change?")
    (right "The box blinks five times as fast")
    (wrong "The box is a fifth of the size"
      "WAIT changes only the time between the steps. The box is drawn by line 20, which you did not change.")
    (wrong "The box stays on"
      "Line 20 still inverts it on every pass. It just blinks faster.")
    (hint "500 milliseconds became 100."))))
```

---

```basic
10 FOR I = 1 TO 8
20 PRINT "*";
30 WAIT 250
40 NEXT I
```

```check
(number
  (ask "About how many seconds does this program take?")
  (answer 2) (from "8 * 250 / 1000")
  (wrong-if 2000 "2000 is the time in milliseconds. In seconds, divide by 1000.")
  (wrong-if 8 "Each wait is a quarter of a second, not a whole one.")
  (hint "8 waits of 250 milliseconds each."))
```

So: `SLEEP` waits whole seconds, `WAIT` waits milliseconds, and waits
set the pace of what a program shows.
