# Guess my number

```lesson
(objective "Make a number the player cannot predict, and write a guessing game with it.")
(minutes 20)
```

In this lesson you write your first game: the computer thinks of a
number from 1 to 100, and you guess it.

A game needs a number the player cannot know in advance. Many BASICs
have a function for that, but Machdyne BASIC 1 has none. A person's
timing can do the job instead: count as fast as possible until a key
is pressed. Nobody can time a key press to stop at the count they want.

```basic
20 PRINT "PRESS A KEY TO START"
30 R = (R + 1) MOD 100
40 IF KEY = 0 THEN 30
```

Lines 30 and 40 go round and round, many times a
second, until a key is pressed. `MOD 100` keeps R from 0 to 99: after
99 comes 0 again.

```sources
(source "Machdyne BASIC 1 reference (sw/ext/basic/docs/basic1.md), sections 4 and 14: its functions and reserved names")
```

```check
(choice
  (ask "Why does line 30 use MOD 100 instead of just R = R + 1?")
  (right "To keep R between 0 and 99, so it never grows too big")
  (wrong "To make R count faster"
    "MOD does not change the speed. It brings R back to 0 after 99.")
  (wrong "Because R + 1 alone is a SYNTAX ERROR"
    "R = R + 1 is fine. But after 32767 it would wrap round to -32768, and the game wants 0 to 99.")
  (hint "What is the largest value R MOD 100 can have?"))
```

---

## From 0-99 to 1-100

R is from 0 to 99. The game wants 1 to 100, so the secret number is
`N = R + 1`.

```check
(text
  (ask "To make a secret number from 1 to 6, like a dice, which number goes after MOD in line 30? (Then N = R + 1.)")
  (accept "6")
  (wrong-if "7" "R MOD 7 can be 0 to 6, which makes N 1 to 7. MOD 6 gives 0 to 5, and N 1 to 6.")
  (wrong-if "5" "R MOD 5 is 0 to 4, so N would be 1 to 5. Six faces need MOD 6.")
  (hint "R MOD 6 is 0, 1, 2, 3, 4 or 5."))
```

---

## The game

```basic
10 REM GUESS MY NUMBER
20 PRINT "PRESS A KEY TO START"
30 R = (R + 1) MOD 100
40 IF KEY = 0 THEN 30
50 N = R + 1: T = 0
60 INPUT "YOUR GUESS"; G
70 T = T + 1
80 IF G < N THEN PRINT "HIGHER": GOTO 60
90 IF G > N THEN PRINT "LOWER": GOTO 60
100 PRINT "RIGHT IN "; T; " GUESSES"
```

T counts the guesses. When the guess is too low, line 80 says
`HIGHER` and jumps back for another; too high, line 90 says `LOWER`.
When neither is true the guess is right, and line 100 says so.

```try
(try (app "basic")
  (task "In BASIC, type NEW, then the game, then RUN. Press a key, play until you win, then SAVE GUESS. Come back.")
  (check (choice
    (ask "What is the best first guess?")
    (right "50: whatever the answer, it rules out half the numbers")
    (wrong "1: start at the bottom and work up"
      "That can take 100 guesses. A guess in the middle halves what is left each time.")
    (wrong "100: start at the top"
      "That rules out just one number. A guess in the middle halves what is left.")
    (hint "Which guess leaves the fewest numbers, whatever the answer?"))))
```

---

## How many guesses

Each wrong guess in the middle of what is left rules out the guess and
half the rest. At worst 100 numbers become 50, 25, 12, 6, 3 and then
1: six guesses, and the seventh is right. *Timeless Computing* tells
more about searching by halving.

```sources
(source "Timeless Computing (docs/tc.md), Thinking in Programs: Efficiency")
```

```check
(number
  (ask "Playing well, guessing in the middle each time, how many guesses do you need at most for a number from 1 to 100?")
  (answer 7) (true "2^6 < 100") (true "2^7 >= 100")
  (wrong-if 50 "Guessing in the middle halves the numbers left each time, so it takes far fewer than 50.")
  (wrong-if 100 "That is guessing one number at a time. Halving each time takes only 7.")
  (hint "After each wrong guess at most half is left: 50, 25, 12, ...")
  (hint "Count the guesses until one number is left, then add the guess that names it."))
```

---

```check
(choice
  (ask "The secret number is 30. You guess 40, then 20. What does the program print after each?")
  (right "LOWER, then HIGHER")
  (wrong "HIGHER, then LOWER"
    "The program tells you which way to go: 40 is too high, so it says LOWER.")
  (wrong "LOWER, then LOWER"
    "20 is below 30, so G < N holds and line 80 says HIGHER.")
  (hint "40 > 30, and 20 < 30. Which line runs for each?"))
```

So: counting until a key is pressed gives a number nobody can predict,
`MOD` brings it into range, and `IF` and `GOTO` make the guessing game.
