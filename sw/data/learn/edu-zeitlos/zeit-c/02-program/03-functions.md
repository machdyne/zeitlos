# Functions

```lesson
(objective "Define and call functions with parameters and a return value, and say why a function gets copies of the values passed to it.")
(minutes 20)
(keywords "parameter (in C)" "return value (in C)" "local variable (in C)")
```

You define a function of your own the way main is defined:

```c
int square(int n)
{
    return n * n;
}
```

`int n` is a **parameter**: a variable that holds the value the caller
gives. `square(7)` runs the function with n set to 7. `return` ends
the function and hands back its **return value**, here 49, which takes
the call's place in the caller:

```c
printf("%d\r\n", square(7) + 1);
```

A function can have several parameters, separated by commas:
`int add(int a, int b)`.

```check
(number
  (ask "What does the printf above print?")
  (answer 50) (from "7 * 7 + 1")
  (wrong-if 49 "square(7) is 49, and the call then adds 1.")
  (wrong-if 64 "The call is square(7), and 1 is added after: 49 + 1, not 8 squared.")
  (hint "Work out square(7) first, then add 1."))
```

---

## void

A function that gives nothing back has the type void, and its
`return;`, if it has one, has no value. `(void)` in place of the
parameters means it takes none.

```c
void line(int n)
{
    for (int i = 0; i < n; i++)
        printf("-");
    printf("\r\n");
}
```

`line(10);` prints ten dashes and a line end. Using a void function's
value, as in `x = line(10);`, is an error.

```check
(choice
  (ask "Which is right for a function that takes no values and gives none back?")
  (right "void beep(void)")
  (wrong "beep()"
    "Every definition starts with the type it gives back. A function that gives nothing back says so: void.")
  (wrong "int beep(void) with no return"
    "int promises a whole number back. A function that gives nothing is declared void.")
  (hint "Nothing back, and nothing in: the same word twice."))
```

---

## Copies

A parameter is a **local variable**: it belongs to that one call of
the function, and it starts as a copy of what the caller passed.

```c
void twice(int n)
{
    n = n * 2;
}
```

After `int x = 5; twice(x);`, x is still 5. twice doubled its own
copy, which disappeared when it returned. To give a result back, use
the return value. (The pointers lesson shows the other way.)

```check
(number
  (ask "int x = 5; twice(x); twice(x); What is x now?")
  (answer 5)
  (wrong-if 20 "twice changes only its parameter, a copy. x itself is never touched.")
  (wrong-if 10 "Even once, twice does not change x: it doubles its own copy.")
  (hint "Does twice ever store anything in x?"))
```

---

## Before it is called

zcc reads your file from top to bottom, once. A function must be
defined, or at least declared, above the first line that calls it. A
declaration is the first line with a semicolon in place of the body:

```c
int square(int n);
```

That is all the printf line in first.c is.

```check
(choice
  (ask "main calls square, and square is defined below main. What must go above main?")
  (right "A declaration: int square(int n);")
  (wrong "Nothing: zcc finds square wherever it is"
    "zcc reads the file once, top to bottom. At the call it must already know square; otherwise it stops with 'square' is not declared.")
  (wrong "A second definition of square"
    "Two definitions of one function is an error. A declaration, ending in a semicolon, is enough.")
  (hint "What did first.c put above main for printf?"))
```

---

## Practice

Euclid's way of finding the largest number that divides two others:

```c
int gcd(int a, int b)
{
    while (b != 0) {
        int t = a % b;
        a = b;
        b = t;
    }
    return a;
}
```

```sources
(source "Timeless Computing (docs/tc.md), Three Languages Side by Side: Greatest Common Divisor")
```

```check
(number
  (ask "What does gcd(12, 18) return?")
  (answer 6) (true "12 / 6 = 2") (true "18 / 6 = 3")
  (wrong-if 3 "3 divides both, but so does a bigger number.")
  (wrong-if 36 "36 is a common multiple. gcd finds the greatest number that divides both.")
  (hint "Follow the loop: a = 12, b = 18. t = 12 % 18 = 12, so a becomes 18 and b 12.")
  (hint "Next t = 18 % 12 = 6, then 12 % 6 = 0."))
```

```check
(number
  (ask "And gcd(1071, 462)?")
  (answer 21) (true "1071 / 21 = 51") (true "462 / 21 = 22")
  (hint "1071 % 462 is 147. Then 462 % 147, and so on.")
  (hint "462 % 147 = 21, and 147 % 21 = 0."))
```

So: a function takes copies of the values passed to it as parameters, returns a
value (or is void), and must be declared before it is called.
