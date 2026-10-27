# Choices and loops

```lesson
(objective "Write if, while and for in C, with conditions that compare and combine.")
(minutes 20)
(keywords "block (in C)" "comparison (in C)")
```

C's IF looks like this:

```c
if (score > 100) {
    printf("high score!\r\n");
} else {
    printf("try again\r\n");
}
```

The condition goes in parentheses, and there is no THEN. Each branch is
a **block**: statements between braces. A block of a single statement
may leave the braces out, but keeping them avoids mistakes when a
second statement is added later.

The **comparisons** are `<` `>` `<=` `>=`, `==` for equal and `!=` for
not equal.

```sources
(source "Timeless Computing (docs/tc.md), Three Languages Side by Side: Common Constructs")
```

```check
(choice
  (ask "Which tests whether x equals 5?")
  (right "if (x == 5)")
  (wrong "if (x = 5)"
    "A single = stores 5 in x, and the value of that is 5, which counts as true: the branch always runs. Equal is ==.")
  (wrong "if x == 5 then"
    "That is close to BASIC. C needs parentheses round the condition and has no then.")
  (hint "In C, = stores a value. Comparing for equal needs two of them."))
```

---

## True and false

C has no separate true and false. A condition is a number: **0 is
false, and any other number is true**. A comparison gives 1 or 0.

Conditions combine with `&&` (and), `||` (or) and `!` (not):

```c
if (age >= 13 && age <= 19)
    printf("a teenager\r\n");
```

`&&` stops as soon as the answer is known: if the left side is false,
the right side is never worked out. `||` stops at the first true side.

```check
(choice
  (ask "x is 0. Which condition is true?")
  (right "!x")
  (wrong "x"
    "0 is false in C, so if (x) does not run its branch.")
  (wrong "x == 1 || x > 0"
    "Both sides are false when x is 0, so || gives false.")
  (hint "! turns false into true. Is 0 true or false?"))
```

---

## while

`while` repeats a block as long as its condition is true. The
condition is tested before each turn, so the block may run no times
at all.

```c
int n = 1;
int steps = 0;
while (n < 1000) {
    n = n * 2;
    steps = steps + 1;
}
printf("%d steps, n is %d\r\n", steps, n);
```

```check
(number
  (ask "How many steps does the loop above count?")
  (answer 10) (true "2^9 < 1000") (true "2^10 >= 1000")
  (wrong-if 9 "After 9 doublings n is 512, still under 1000, so the loop goes round once more.")
  (wrong-if 1000 "steps counts the turns of the loop, not the value of n.")
  (hint "n goes 2, 4, 8, ... Count the doublings until n reaches 1000 or more."))
```

---

## for

`for` puts a counting loop on one line: a start, a condition, and a
step, separated by semicolons.

```c
for (int c = 0; c <= 100; c += 20)
    printf("%d %d\r\n", c, c * 9 / 5 + 32);
```

This prints Celsius and Fahrenheit, as BASIC's `FOR C = 0 TO 100 STEP
20` would. `c += 20` is short for `c = c + 20`; `i++` adds 1 to i and
`i--` takes 1 away.

Multiply before dividing: `c * 9 / 5` loses nothing here, but
`c / 5 * 9` would drop a fraction for any c that is not a multiple of 5.

```sources
(source "Timeless Computing (docs/tc.md), Three Languages Side by Side: A Conversion Table")
```

```check
(number
  (ask "How many lines does that for loop print?")
  (answer 6)
  (wrong-if 5 "c takes the values 0, 20, 40, 60, 80 and 100. 100 <= 100 is true, so the last one is printed too.")
  (hint "List the values of c: start at 0, add 20 while c <= 100."))
```

---

## The Collatz loop

An even number is halved and an odd one becomes 3n + 1, until it
reaches 1. In C:

```c
int collatz(int n)
{
    int steps = 0;
    while (n != 1) {
        if (n % 2 == 0)
            n = n / 2;
        else
            n = 3 * n + 1;
        steps++;
    }
    return steps;
}
```

```example
(example
  (problem "How many steps does collatz(6) take?")
  (step "6 is even: 3. 3 is odd: 10. 10 is even: 5.")
  (step "5 is odd: 16. Then 8, 4, 2, 1.")
  (step (number (ask "Count the steps: how many arrows from 6 to 1?") (answer 8)
          (wrong-if 9 "Count the arrows, not the numbers: 6, 3, 10, 5, 16, 8, 4, 2, 1 is nine numbers and eight steps.")))
  (answer "8 steps"))
```

```check
(number
  (ask "How many steps does collatz(5) take?")
  (answer 5)
  (wrong-if 6 "5, 16, 8, 4, 2, 1: count the arrows between them, not the numbers.")
  (hint "5 is odd: 3 \\* 5 + 1. Keep going until you reach 1."))
```

So: if, while and for take a condition in parentheses, 0 is false and
anything else true, and == compares where = stores.
