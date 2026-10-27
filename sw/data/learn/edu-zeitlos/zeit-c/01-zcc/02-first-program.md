# A first program

```lesson
(objective "Read every part of a small C program, and type it into a source file.")
(minutes 20)
(keywords "function (in C)" "data type" "int (in C)" "void (in C)" "character (of text)" "statement (in C)" "string (in C)" "comment (in C)" "declaration (in C)" "console (of Zeitlos)")
```

Here is a whole C program. It prints two lines.

```c
/* first.c: my first C program */

int printf(const char *format, ...);

int main(void)
{
    printf("Hello from C!\r\n");
    printf("2 + 3 = %d\r\n", 2 + 3);
    return 0;
}
```

The first line is a **comment**: everything between `/*` and `*/` is
for people, and zcc skips it. `//` starts a comment that runs to the
end of the line.

The rest of this lesson takes the program apart, one piece at a time.

```check
(choice
  (ask "What does zcc do with the line /\\* first.c: my first C program \\*/ ?")
  (right "Skips it: it is a comment, for people reading the source")
  (wrong "Prints it when the program starts"
    "Only printf prints. Text between /\\* and \\*/ is a comment, and zcc leaves it out of the program entirely.")
  (wrong "Uses it as the program's name"
    "The program's name is whatever you tell zcc to call the file it makes. A comment has no effect on the program.")
  (hint "Look at the marks around it: /\\* and \\*/."))
```

---

## main

`main(void) { ... }` is a **function**: a named piece of program, like
the lines a GOSUB runs in BASIC. A function can be given values and can give one
back. Every C program has a function called **main**, and that is
where it starts.

- `int` before `main` is a **data type**: it says what kind of value
  something is. **int** means a whole number, so main gives back a
  whole number. The last line, `return 0;`, gives back 0, which tells
  the system that the program succeeded.
- **void** means nothing: `(void)` says main is given no values.
- The braces `{ }` hold main's **statements**: the instructions, run
  in order from the top. Each statement ends with a semicolon.

```check
(number
  (ask "How many statements does main have in first.c?")
  (answer 3)
  (wrong-if 2 "Count the return too: return 0; is a statement, and it ends with a semicolon like the other two.")
  (wrong-if 4 "The line int printf(...); is outside main's braces. Count only what is between { and }.")
  (hint "Count the semicolons between main's { and }."))
```

---

## printf and strings

`printf` prints. What it prints is a **string**: text between double
quotes. Two things inside a string are not printed as they are:

- `%d` is a place for a number: the next value after the string, here
  `2 + 3`, is worked out and printed there in digits.
- `\r\n` ends the line. `\r` moves back to the start of the line and
  `\n` moves down one line. A backslash and a letter stand for one
  **character** (a letter, digit, space or sign of text) that has no
  key of its own.

On Zeitlos your program's printing goes to the **console**: the
system's own record of printed text, which you open in `term` in the
next lesson. There a line needs both: with `\n` alone, the next line
starts below the end of this one, not at the left.

```check
(text
  (ask "What does printf(\"%d\\r\\n\", 4 \\* 5); print, before the end of the line?")
  (accept "20")
  (wrong-if "4 * 5" "printf works out the value first: %d is replaced by the number 4 \\* 5 is.")
  (wrong-if "%d" "%d is never printed itself: it is the place where the next value goes.")
  (hint "%d is replaced by the value after the string. What is 4 \\* 5?"))
```

---

## The declaration

The line before main,

```c
int printf(const char *format, ...);
```

is a **declaration**: it tells zcc that a function called printf
exists, what it gives back (`int`) and what it takes: a string, then
`...`, any number of further values. The declaration has a semicolon
and no braces: printf itself is in libz, which zcc copies into your
program.

zcc must meet a declaration before the first call. Without this line,
it stops with `'printf' is not declared`.

Large programs keep their declarations in files of their own, which
the next unit shows. On the board, writing the few you need yourself is
also the fastest way: zcc reads a declaration of one line instead of a
file of hundreds.

```sources
(source "docs/zcc.md, Diagnostics; Fixes found by the demo (October 2026)")
(source "docs/libz.md, What is in it, and what is not")
```

```check
(choice
  (ask "Why does first.c declare printf, though it never defines what printf does?")
  (right "zcc must know printf's name and what it takes before the program calls it; the function itself comes from libz")
  (wrong "The declaration is what prints the text"
    "A declaration does nothing when the program runs. It only tells zcc that printf exists and how it is called. The printing is done by printf, in libz.")
  (wrong "It is a comment for people"
    "Comments are between /\\* and \\*/. Leave the declaration out and zcc stops with an error: 'printf' is not declared.")
  (hint "What does zcc say if the line is missing?"))
```

---

## Type it in

```try
(try (app "files")
  (task "Open files from the dock, go to the folder /home, and make a folder there called c with the New Dir button. Then come back.")
  (check (choice (ask "Is there now a folder c in /home?")
    (right "Yes")
    (wrong "No: New Dir is not there" "New Dir is one of the buttons of files; Tab moves between them. Type c as its name and press Enter."))))
```

Then type first.c into `text`, exactly as shown at the top of this
lesson. Tab puts in four spaces. Save it with **Ctrl+S**,
in the folder c, as `first.c`: so its full name is `/home/c/first.c`.

```try
(try (app "text")
  (task "Open text from the dock, type first.c, and save it as first.c in /home/c. Then come back.")
  (check (choice (ask "Does the title of the text window now say first.c?")
    (right "Yes")
    (wrong "No, it still has no name" "Press Ctrl+S, go into the folder c in the dialog, and type first.c as the name."))))
```

So: a C program is functions made of statements, it starts at main,
and it declares what it uses before it calls it.
