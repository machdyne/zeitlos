# Answering questions

```lesson
(objective "Answer each kind of question learn asks, and use hints.")
(minutes 10)
```

Some questions give you answers to choose from. Press the number of an
answer, or click it.

If you choose a wrong answer, learn tells you why it is wrong, and you
can try again.

```check
(choice
  (ask "What is" (math "3 + 4") "?")
  (right "7" (true "3 + 4 = 7"))
  (wrong "34" (false "3 + 4 = 34") "That is 3 and 4 written side by side. Adding them makes 7.")
  (wrong "1" (false "3 + 4 = 1") "That is 4 take away 3. Adding makes more, not less."))
```

---

Other questions want you to **type** the answer, then press Enter.

For a fraction, type it with a slash: $3/4$ is typed `3/4`.

```check
(fraction
  (ask "Type one half as a fraction.")
  (answer 1 2)
  (wrong-if (2 1) "That is two over one, which is 2. A half is one over two.")
  (hint "One part out of two equal parts."))
```

---

## Hints

Stuck? Press **H** for a hint. There can be up to three, each a little
more help than the one before.

A question answered after its hints is not counted as known: it comes
back later, so you can learn it properly.

```check
(number
  (ask "How many hints can a question have, at most?")
  (answer 3)
  (wrong-if 1 "There can be more than one: each gives a little more help.")
  (hint "Think of a ladder: a small step, a bigger one, then nearly the answer.")
  (hint "It is more than two."))
```
