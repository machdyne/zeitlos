# Your progress

```lesson
(objective "Know where learn keeps your progress, and how to report a mistake.")
(minutes 5)
```

learn keeps what you have done as you go: each lesson finished, and
where you stopped in one you left.

| Where | When |
|---|---|
| on this machine | a learner made here |
| on your USB stick | a learner kept on a stick |

Your progress is plain text that you can read yourself. A parent or
teacher can keep it safe with **backup**.

---

## A mistake in a lesson?

Lessons are checked carefully, but if you find a mistake, press **F7**
on that step and say what is wrong. Your report is kept with your
progress, ready to send.

```example
(example
  (problem "You think a lesson says 2 + 2 = 5.")
  (step "Press F7 on that step.")
  (step "Type what is wrong: 2 + 2 is 4, not 5.")
  (step "Press Enter. The report is kept, and the lesson carries on.")
  (answer "The mistake is reported, in your own words."))
```

```check
(choice
  (ask "Which key reports a mistake in a lesson?")
  (right "F7")
  (wrong "F5" "F5 gives a hint for the question.")
  (wrong "F6" "F6 reads the step aloud.")
  (hint "The keys in a lesson go F5 hint, F6 say, then the one for a mistake."))
```
