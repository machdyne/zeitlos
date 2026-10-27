# Halves

```lesson
(objective "Know that a half is one of two equal parts.")
(standards "3.NF.A.1")
(minutes 15)
```

Cut a shape into two parts of the same size. Each part is **one half**,
written $1/2$.

![a square cut in half](img/half.png)

```figure
(half-bar 0 'warm)
```

---

```check
(choice
  (ask "Which of these is a half?")
  (right "one of two equal parts" (true "1/2 + 1/2 = 1"))
  (wrong "one of two parts of any size"
    "The two parts must be the same size. Otherwise one is more than a half.")
  (wrong "one of three equal parts" (false "1/3 = 1/2")
    "Three equal parts are thirds. A half is one of two.")
  (hint "Picture a sandwich cut so that two friends get the same."))
```

---

Two halves make a whole: $1/2 + 1/2 = 1$.

```check
(number
  (ask "How many halves make one whole?")
  (answer 2)
  (from "1 / (1/2)")
  (wrong-if 1 "One half is only part of the whole. How many make all of it?"))
```
