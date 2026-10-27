# Compile and run

```lesson
(objective "Compile a source file with zcc in term, run the program, and see what it prints.")
(minutes 20)
(keywords "shell (posix)" "option (of a command)")
```

zcc has no window of its own: you give it commands in `term`.

Open `term` from the dock. Its panel offers several shells; click
**POSIX** (or Tab to it and press Enter). A **shell** is a program that
runs the commands you type, one line at a time. POSIX's shell shows a
`$` prompt.

Two of its commands help at once:

- `cd /home/c` makes /home/c the shell's current folder;
- `ls` lists what is in it. You should see `first.c`.

If the POSIX button is disabled and the panel says `not installed`,
this card has no posix; it needs about 4 MB of memory as well.

```sources
(source "docs/terminal.md, Starting out: the panel")
(source "docs/posix.md, Getting there; The shell")
```

```check
(choice
  (ask "You typed ls and the list does not show first.c. What is the likely reason?")
  (right "The shell's current folder is not /home/c, or the file was saved somewhere else")
  (wrong "zcc has not compiled it yet"
    "ls lists files, whatever made them. first.c is the source you saved in text; it is there before any compiling.")
  (wrong "ls only lists programs"
    "ls lists every file and folder in the current folder. If first.c is missing, look at where you are and where you saved it.")
  (hint "ls shows the current folder. Which folder did you cd into, and where did text save the file?"))
```

---

## The zcc command

Type this, all on one line, and press Enter:

```text
zcc /home/c/first.c -o /home/c/first
```

The first name is the source file. `-o` is an **option**: a word that
starts with `-` and changes what a command does. `-o` says what to call
the program zcc makes, and the name after it is that file.

Give zcc full names, starting with `/`. The shell passes zcc the words
you typed but not its current folder, so `first.c` alone would be
looked for at the top of the card.

If all is well, zcc prints nothing and the `$` comes back. If the
shell says `run: zcc: cannot start`, the card does not have zcc: it
comes with the Development set ([updating](docs:update)).

```sources
(source "docs/zcc.md, Command line; The card's layout is the default search path")
(source "docs/demo.md, The languages demo: absolute paths for zcc")
(source "sw/apps/posix/sh.c, bi_run(): the arguments are passed as typed")
```

```check
(text
  (ask "Type the command that compiles /home/c/sum.c into a program /home/c/sum.")
  (accept "zcc /home/c/sum.c -o /home/c/sum")
  (accept "zcc -o /home/c/sum /home/c/sum.c")
  (accept "run zcc /home/c/sum.c -o /home/c/sum")
  (accept "run zcc -o /home/c/sum /home/c/sum.c")
  (wrong-if "zcc sum.c -o sum" "zcc is not told the shell's current folder, so it would look for /sum.c. Use the full names, starting with /home/c/.")
  (wrong-if "zcc /home/c/sum.c" "Without -o, zcc names the program a.bin. Add -o and the name you want: -o /home/c/sum.")
  (hint "zcc, the source file, then -o and the program's name.")
  (hint "Both names are full: /home/c/sum.c and /home/c/sum."))
```

---

## Where printing goes

A program you compile prints to the **console**, not to the term
window you typed in. Open a second term: click `term` in the dock
again. On its panel click **CONSOLE**.

That window shows the last part of what the system has printed since
it started, then whatever is printed next. Leave it open beside the first one.
What you type there goes to the system's own shell, so you need not
type there at all.

```sources
(source "docs/console.md, What you get")
(source "docs/posix.md, Gotchas worth knowing: a program's output does not come back to the shell unless the program asks")
```

```check
(choice
  (ask "zcc's error messages appear in your POSIX window, but your own program's printf does not. Why?")
  (right "zcc asks the shell to show its output; a program that does not ask prints to the console")
  (wrong "printf is broken in zcc's programs"
    "printf works: its text goes to the console. Open a term with CONSOLE to read it.")
  (wrong "The program did not run"
    "It may well have run. Look in a term window showing CONSOLE before deciding.")
  (hint "Where does a program's printing go on Zeitlos, unless it arranges otherwise?"))
```

---

## Run it

Back in the POSIX window, with /home/c as the current folder, type
the program's name:

```text
first
```

The shell looks in its current folder first, so `first` runs the
program you just made. `/home/c/first` works from anywhere. The two
lines appear in the CONSOLE window, and the `$` comes back when the
program has finished.

```try
(try (app "term")
  (task "Open term from the dock, connect one window to POSIX and one to CONSOLE. In POSIX compile first.c with zcc and run first. Then come back.")
  (check (text (ask "What is the second line the program printed?")
    (accept "2 + 3 = 5")
    (accept "2+3=5")
    (wrong-if "2 + 3 = %d" "printf replaces %d with the value of 2 + 3; look again in the CONSOLE window.")
    (hint "printf(\"2 + 3 = %d\\r\\n\", 2 + 3) prints the text with %d replaced by the sum."))))
```

---

## Both at once

The shell's `&&` runs a second command only if the first one
succeeded. zcc fails when it finds a mistake, so

```text
zcc /home/c/first.c -o /home/c/first && first
```

compiles, and runs the program only when there is a new one to run.
The shell keeps the lines you typed: Up brings the last one back, so
the whole loop is: change the source in text, save, Up, Enter.

```sources
(source "docs/posix.md, Gotchas worth knowing: && waits for the program to finish")
```

```check
(choice
  (ask "You type zcc /home/c/first.c -o /home/c/first && first, and zcc finds a mistake. What happens?")
  (right "zcc reports the mistake, and first is not run")
  (wrong "first runs anyway, as the old program"
    "&& runs the second command only when the first succeeds. zcc failed, so first does not run.")
  (wrong "first runs with the mistake in it"
    "When zcc finds a mistake it makes no program at all. And && stops before first because zcc failed.")
  (hint "What does && check before it runs the second command?"))
```

So: compile with zcc and full names, run the program by its name, and
read its printing in a CONSOLE window.
