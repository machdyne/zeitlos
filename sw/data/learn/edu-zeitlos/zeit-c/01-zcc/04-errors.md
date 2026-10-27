# When zcc says no

```lesson
(objective "Read a zcc error message, find the mistake it points to, and fix it.")
(minutes 15)
(keywords "error message (of zcc)")
```

When zcc finds a mistake it stops, makes no program, and prints an
**error message** in the POSIX window:

```text
/home/c/first.c:7:11: error: 'prnitf' is not declared
```

It has four parts, separated by colons:

| part | here | says |
|---|---|---|
| file | /home/c/first.c | which source file |
| line | 7 | which line, counting from 1 |
| column | 11 | which character on that line: here, the one just after the name |
| text | 'prnitf' is not declared | what zcc could not accept |

Here `printf` was typed as `prnitf`, a name zcc has never been told
about.

```sources
(source "docs/zcc.md, Diagnostics")
```

```check
(number
  (ask "zcc prints /home/c/sum.c:12:9: error: expected ';', found 'return'. On which line does zcc say it noticed the mistake?")
  (answer 12)
  (wrong-if 9 "9 is the column: the character on the line. The line is the number just after the file name.")
  (hint "The numbers come in the order file, line, column."))
```

---

## Look one line up

A missing semicolon is the commonest mistake, and zcc notices it late:

```c
    int x = 3
    printf("%d\r\n", x);
```

zcc says `expected ';', found 'printf'`, on the printf line. Line
breaks mean nothing to C, so zcc only knows the statement has not
ended when it reaches the next word. The fix is at the end of the line
**before**.

```check
(choice
  (ask "zcc says line 8: expected ';', found 'printf'. Line 8 is a correct printf. Where is the mistake most likely?")
  (right "At the end of line 7, which is missing its semicolon")
  (wrong "In the printf on line 8"
    "zcc reports where it noticed, which is the first word after the unfinished statement. Line 8 can be perfect.")
  (wrong "In the declaration of printf"
    "A wrong declaration gives a different error message. expected ';' means a statement before this point did not end.")
  (hint "zcc expected a semicolon and found the start of line 8 instead. What came just before it?"))
```

---

## One at a time

zcc reports **only the first** mistake and stops. After a mistake, a
compiler that goes on is guessing where it is, and its next reports
are often wrong. So the loop is: fix that one, compile again, and read
the next error message if there is one.

Error messages you will meet:

| it says | usual cause |
|---|---|
| `'name' is not declared` | a typing slip, or a missing declaration |
| `expected ';', found ...` | the line before has no semicolon |
| `'name' is declared but never defined` | a declaration of a function that is in neither your file nor libz |
| `floating-point arithmetic is not supported` | a `float` or `double` in a calculation |

```sources
(source "docs/zcc.md, Diagnostics: there is no error recovery")
```

```check
(choice
  (ask "A program has three mistakes. How many error messages does one run of zcc print?")
  (right "One: the first, then it stops")
  (wrong "Three, one for each mistake"
    "zcc stops at the first mistake. One true report is worth more than several guesses, so it does not go on.")
  (wrong "None: it makes the program and leaves out the bad lines"
    "zcc never makes a program with a mistake in it. It reports the first mistake and stops.")
  (hint "What does zcc do right after it finds a mistake?"))
```

---

## Practice

```check
(choice
  (ask "zcc prints first.c:6:11: error: 'printf' is not declared. The program uses printf correctly on line 6. What is missing?")
  (right "The declaration of printf, before main")
  (wrong "A semicolon at the end of line 5"
    "A missing semicolon gives expected ';'. 'not declared' means zcc has never been told the name.")
  (wrong "libz, which is not on the card"
    "zcc complains about libz in its own words. 'not declared' is about your source: the name was never declared before it was used.")
  (hint "'not declared': which line tells zcc that printf exists?"))
```

```check
(choice
  (ask "zcc prints sum.c:5:14: error: floating-point arithmetic is not supported. What do you change?")
  (right "Use whole numbers: an int, perhaps counting in smaller units")
  (wrong "Nothing: compile again and it will pass"
    "zcc gives the same answer for the same source. It has no floating point at all, so the calculation must use whole numbers.")
  (wrong "Add a declaration for float"
    "float is a data type, not a function: there is nothing to declare. zcc refuses arithmetic on it entirely.")
  (hint "zcc has whole numbers only."))
```

So: an error message names the file, the line and the column; fix
that one mistake, often on the line before, and compile again.
