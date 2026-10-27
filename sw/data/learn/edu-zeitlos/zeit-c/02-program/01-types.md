# Numbers and types

```lesson
(objective "Declare variables of the types int, char and unsigned, and work out whole-number arithmetic as C does it.")
(minutes 20)
(keywords "bit (binary digit)" "byte (8 bits)" "char (in C)" "unsigned (in C)" "remainder (of a division)")
```

In BASIC a variable is ready the first time you use it. In C you
declare every variable first, with its data type: what kind of
value it holds, and so how much memory it takes.

```c
int count = 0;
char letter = 'A';
```

`int count` makes a variable called count that holds a whole number,
and `= 0` gives it its first value. A variable declared inside a
function lives until the function returns. Using a name you never
declared stops zcc with `'count' is not declared`.

```check
(choice
  (ask "Which line makes a whole-number variable called total that starts at 10?")
  (right "int total = 10;")
  (wrong "total = 10;"
    "That only stores 10 in total, so total must already be declared. In C a variable needs a declaration with its type first.")
  (wrong "total int = 10;"
    "The type comes first, then the name: int total.")
  (hint "Type, name, = first value, semicolon."))
```

---

## int

Memory holds **bits**: each bit is a 0 or a 1. An `int` is 32 bits on
Zeitlos, which gives it 2 to the power 32 different values, about 4
billion: from -2,147,483,648 to 2,147,483,647.

That is the whole-number type you use most of the time. `printf`'s
`%d` prints an int.

```c
int seconds = 3600 * 24;
printf("%d seconds in a day\r\n", seconds);
```

```sources
(source "docs/zcc.md, The compiler's own headers: int and long are both 32 bits")
(source "Timeless Computing (docs/tc.md), C: Types")
```

```check
(number
  (ask "How many seconds are in a day, as the program above prints it?")
  (answer 86400) (from "3600 * 24")
  (wrong-if 3600 "3600 is the seconds in an hour. The program multiplies by 24.")
  (hint "3600 seconds an hour, 24 hours a day."))
```

---

## char: a byte

Eight bits make a **byte**, 256 different values. A **char** is one
byte. It holds a character as its number: `'A'`, in single quotes,
is the number 65. `%c` prints a char as its character, `%d` as its number.

On Zeitlos a char is **unsigned**: it holds 0 to 255, never a negative
number. Move the slider to see the byte for each character.

```figure
(define n (slider 'code 32 126 65 "Number" 1))
(text 160 40 (string-append "'" (string (integer->char n)) "' is " (fmt n 0)) 'centre 'big)
(zc-bits 40 80 n)
(caption "a char: 8 bits, one byte")
```

```sources
(source "docs/zcc.md, What it caught: plain char is unsigned on RISC-V")
```

```check
(number
  (ask "'B' is 66. What number is 'C'?")
  (answer 67) (from "66 + 1")
  (wrong-if 3 "'C' is the third letter, but its number in a char is its character code, which follows 'B' at 66.")
  (hint "The letters follow each other: 'A' is 65, 'B' is 66."))
```

---

## unsigned

`unsigned int` (or just `unsigned`) gives up negative numbers for
twice the positive range: 0 to 4,294,967,295. `%u` prints one.

At its largest, adding 1 to an unsigned wraps round to 0, like the digits
of a car's mileage display. For a plain int, C does not say what happens when a
value goes past its range, so a correct program never lets it.

```check
(choice
  (ask "An unsigned variable holds 0. The program subtracts 1. What does it hold now?")
  (right "4,294,967,295, its largest value")
  (wrong "-1"
    "An unsigned has no negative numbers. Going below 0 wraps round to the top of its range.")
  (wrong "0: it cannot go lower"
    "C does not stop at 0. An unsigned wraps round, like a mileage display rolling back from 0000 to 9999.")
  (hint "Think of a mileage display turning backwards past zero."))
```

---

## Whole-number arithmetic

`+ - *` work as usual. `/` between whole numbers gives a whole number:
the fraction is dropped. `%` gives the **remainder** of the division.

```example
(example
  (problem "Turn 200 seconds into minutes and seconds.")
  (step "200 / 60 is 3: the 0.33 is dropped.")
  (step "200 % 60 is 20: what is left after 3 whole minutes, 180 seconds.")
  (step (number (ask "So 200 seconds is 3 minutes and how many seconds?") (answer 20) (from "200 - 3 * 60")
          (wrong-if 33 "The dropped fraction is not the seconds. % gives what is left over: 200 - 180.")))
  (answer "3 minutes 20 seconds"))
```

```check
(number
  (ask "What is 17 / 5 in C, with two int values?")
  (answer 3) (true "3 * 5 <= 17") (true "4 * 5 > 17")
  (wrong-if 3.4 "C drops the fraction when both values are whole numbers. 17 / 5 is 3.")
  (wrong-if 4 "C does not round up; it drops the fraction. 3.4 becomes 3.")
  (hint "5 goes into 17 how many whole times?"))
```

```check
(number
  (ask "And 17 % 5?")
  (answer 2) (from "17 - 3 * 5")
  (wrong-if 3 "3 is 17 / 5. % is what is left over: 17 - 15.")
  (hint "3 fives make 15. How much of 17 is left?"))
```

So: every variable has a type; int is 32 bits, char is one unsigned
byte, and whole-number division drops the fraction.
