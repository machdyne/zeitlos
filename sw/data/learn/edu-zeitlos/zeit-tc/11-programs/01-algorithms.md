# Algorithms

```lesson
(objective "Say what an algorithm is, and follow Euclid's algorithm step by step.")
(minutes 15)
(keywords "algorithm" "pseudocode")
```

A program is a precise description of how to do a task, written for a
machine that does exactly what it is told and nothing else. Learning to
program is mostly learning to divide a task into steps small and clear
enough for a machine to follow.

You can use a computer without programming it. But a person who can
write short programs can make it do tasks nobody planned for: renaming
a thousand files, logging a temperature every hour, controlling a pump.
And they can keep their tools working without depending on anyone else.

```sources
(source "Timeless Computing (docs/tc.md), Thinking in Programs; Why Program")
```

```check
(choice
  (ask "What is most of the work of learning to program?")
  (right "Dividing a task into steps small and clear enough for a machine")
  (wrong "Learning the words of a programming language by heart"
    "A language's words are learned in days. The lasting skill is breaking a task into exact steps, which carries over to every language.")
  (wrong "Learning how to type fast"
    "Programs are short compared with the thinking behind them. The hard part is deciding the steps, not typing them.")
  (hint "The machine does exactly what it is told. What must you be able to do, then?"))
```

---

## Algorithms

An **algorithm** is a method for solving a problem in a finite number
of exact steps. Algorithms are older than computers. One of the oldest
known, recorded by Euclid around 300 BC, finds the
**greatest common divisor** of two numbers: the largest number that divides both exactly.

```text
to find the greatest common divisor of A and B:
    while B is not zero:
        let R be the remainder of A divided by B
        replace A with B, and B with R
    the answer is A
```

Steps in plain words like these are **pseudocode**: a way to plan a
program before choosing a language.

```check
(number
  (ask "What is the greatest common divisor of 12 and 18? (You can work it out without the algorithm.)")
  (answer 6) (true "12/6 = 2") (true "18/6 = 3")
  (wrong-if 3 "3 divides both, but so does a larger number.")
  (wrong-if 2 "2 divides both, but so does a larger number.")
  (wrong-if 36 "36 is the smallest number both divide into. The divisor must divide both 12 and 18.")
  (hint "List the numbers that divide 12: 1, 2, 3, 4, 6, 12. Which of them also divide 18?"))
```

---

## Watching it run

Pick two numbers and move the slider: each step divides, keeps the
remainder, and moves along.

```figure
(define p (choice 'p '("1071, 462" "48, 18" "35, 12") "A, B"))
(define ab (list-ref '((1071 462) (48 18) (35 12)) p))
(define s (slider 's 0 4 0 "Steps" 1))
(text 70 30 "A" 'centre 'body 'bold)
(text 150 30 "B" 'centre 'body 'bold)
(text 240 30 "remainder" 'centre 'body 'bold)
(line 30 38 290 38 'ink 1)
(let loop ((i 0) (a (car ab)) (b (cadr ab)))
  (if (and (< i s) (> b 0))
      (begin
        (text 70 (+ 60 (* i 24)) (fmt a 0) 'centre 'body)
        (text 150 (+ 60 (* i 24)) (fmt b 0) 'centre 'body)
        (text 240 (+ 60 (* i 24)) (fmt (modulo a b) 0) 'centre 'body)
        (loop (+ i 1) b (modulo a b)))
      (begin
        (text 70 (+ 60 (* i 24)) (fmt a 0) 'centre 'body)
        (text 150 (+ 60 (* i 24)) (fmt b 0) 'centre 'body)
        (caption (if (= b 0)
                     (string-append "B is zero: the answer is " (fmt a 0))
                     "B is not zero: divide again")))))
```

```check
(number
  (ask "Follow the algorithm for 48 and 18. What is the greatest common divisor?")
  (answer 6) (true "48 - 2*18 = 12") (true "18 - 12 = 6") (true "12/6 = 2")
  (wrong-if 12 "12 is the first remainder. Keep going: 18 divided by 12 leaves 6, and 12 divided by 6 leaves 0.")
  (wrong-if 0 "0 is the last remainder, which ends the repeating. The answer is A at that point.")
  (hint "48 divided by 18 leaves 12. Now A is 18 and B is 12.")
  (hint "18 divided by 12 leaves 6; 12 divided by 6 leaves 0. What is A when B is 0?"))
```

---

```example
(example
  (problem "Find the greatest common divisor of 1071 and 462.")
  (step "1071 divided by 462 is 2, remainder 147. Now A = 462, B = 147.")
  (step "462 divided by 147 is 3, remainder 21. Now A = 147, B = 21.")
  (step "147 divided by 21 is 7, remainder 0. Now A = 21, B = 0.")
  (step (choice (ask "B is zero. What is the answer?")
          (right "21")
          (wrong "0" "0 is B, the last remainder. The algorithm says the answer is A.")
          (wrong "147" "147 was A one step ago. After the last division, A is 21.")))
  (answer "The greatest common divisor of 1071 and 462 is 21."))
```

---

```check
(number
  (ask "Use the algorithm on 35 and 12. What is the greatest common divisor?")
  (answer 1) (true "35 - 2*12 = 11") (true "12 - 11 = 1")
  (wrong-if 11 "11 is the first remainder. 12 divided by 11 leaves 1, and 11 divided by 1 leaves 0.")
  (wrong-if 0 "0 is the last remainder. The answer is A when B reaches 0.")
  (hint "35 divided by 12 leaves 11. Then 12 divided by 11 leaves 1.")
  (hint "11 divided by 1 leaves 0, so the answer is A."))
```

---

```check
(choice
  (ask "Why must an algorithm have a finite number of steps?")
  (right "A method that never finishes never gives an answer")
  (wrong "Because computers have a limited memory"
    "Even with unlimited memory, a method that runs forever gives no answer. Finishing is part of what makes it a method.")
  (hint "In Euclid's algorithm, what makes the repeating stop?"))
```

So: an algorithm is a finite list of exact steps, and pseudocode is a
way to write one down before choosing a language.
