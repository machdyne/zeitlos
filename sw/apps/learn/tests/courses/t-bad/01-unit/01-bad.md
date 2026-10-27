# Wrong answers

```lesson
(minutes 90)
```

Greek in text: π is not drawable. A bad formula: $3 + $ and $(1 + 2$.
A broken link [here](lesson:nowhere/at-all) and [there](ftp://x).

![missing](img/none.png)

```check
(choice
  (ask "Pick one")
  (right "a")
  (right "b")
  (wrong "c")
  (wrong "a" "Same as a right one."))
```

```check
(number
  (ask "2 pi sqrt(0.5/9.81)?")
  (answer 1.5)
  (from "2 pi sqrt(0.5/9.81)")
  (within 0.01))
```

```check
(choice
  (ask "Claims")
  (right "yes" (true "2/5 > 2/3"))
  (wrong "no" (false "1 + 1 = 2") "feedback"))
```

```check
(essay (ask "Write a story"))
(text (ask "Say it"))
(fraction (ask "Half") (answer 1 0))
```

```try
(try (app "rm") (task "Delete things"))
```

```figure
(frac-bar 0 2 1 'warm "1/2"
```
