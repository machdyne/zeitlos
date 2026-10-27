# Keeping your programs

```lesson
(objective "Save a program, list the saved programs, load one back, and get help.")
(minutes 15)
```

In this lesson you keep a program so that it is still there tomorrow.

The program you type lives in BASIC's memory. When you close BASIC, or
type `NEW`, it is gone. To keep it, you **save** it: BASIC
writes it into a file.

```basic
SAVE HELLO
```

saves the program as the file `HELLO.BAS`. A name is up to 8 letters
and digits; BASIC adds `.BAS` when you give no ending of your own.
BASIC keeps all its files in one folder, `/data/basic`, and nowhere
else.

```sources
(source "docs/basic_app.md, Files")
(source "Machdyne BASIC 1 reference (sw/ext/basic/docs/basic1.md), section 9")
```

```check
(text
  (ask "You type SAVE ROCKET. What is the name of the file BASIC writes?")
  (accept "ROCKET.BAS")
  (wrong-if "ROCKET" "BASIC adds an ending to a name that has none. Which one?")
  (hint "BASIC adds a dot and three letters to a name without an ending.")
  (hint "The three letters are BAS."))
```

---

## Loading it back

**DIR** lists the saved files. To bring a program back, you **load** it:

```basic
LOAD HELLO
```

`LOAD` **replaces** the program in memory with the saved one. Whatever
you had typed and not saved is lost, so save first.

| Command | Does |
|---|---|
| `SAVE name` | writes the program into a file |
| `LOAD name` | replaces the program with a saved one |
| `DIR` | lists the saved files |
| `TYPE name` | shows what is in a file |
| `DEL name` | deletes a file |

```check
(choice
  (ask "You are working on a new program, not yet saved. You type LOAD HELLO. What happens to your new program?")
  (right "It is replaced by HELLO, and lost")
  (wrong "HELLO is added to the end of it"
    "LOAD does not join programs: it replaces the one in memory with the saved one.")
  (wrong "BASIC saves it first"
    "BASIC does not save anything by itself. Save your work before you load another program.")
  (hint "Does LOAD add, or replace?"))
```

---

```try
(try (app "basic")
  (task "In BASIC, type NEW, then 10 PRINT \"SAVED\". Type SAVE TEST1, then NEW, then LIST: the program is gone. Now type LOAD TEST1 and LIST again. Come back.")
  (check (choice
    (ask "What did the second LIST show?")
    (right "10 PRINT \"SAVED\"")
    (wrong "Nothing"
      "Did you type LOAD TEST1 before the second LIST? LOAD brings the saved program back.")
    (wrong "NOT FOUND"
      "NOT FOUND means there is no such file. Check that SAVE TEST1 and LOAD TEST1 use the same name.")
    (hint "LOAD brings back what SAVE wrote."))))
```

---

## A program that starts by itself

A program saved as `BOOT` (the file `BOOT.BAS`) runs by itself each
time BASIC starts. Use it for a program you always want: a welcome, a
clock, a game.

```check
(text
  (ask "What name do you save a program under so that it runs by itself when BASIC starts?")
  (accept "BOOT") (accept "BOOT.BAS")
  (hint "What starting a computer is called, in four capitals: B... ."))
```

---

## Help

- **F1** shows a short help page on BASIC's own screen: its keys, and
  how to write and run a program.
- The command `HELP` prints every command and every name BASIC knows.
- [The BASIC page](docs:basic_app) in the Zeitlos documentation tells
  the whole story.

```check
(choice
  (ask "You forget how BASIC spells the statement that draws a circle. Which command lists every name BASIC knows?")
  (right "HELP")
  (wrong "DIR"
    "DIR lists your saved files, not the names BASIC knows.")
  (wrong "LIST"
    "LIST shows your program. HELP shows every command and statement BASIC knows.")
  (hint "It is what you would call out if you were stuck."))
```

---

```check
(choice
  (ask "You type SAVE MY GAME. Why does BASIC answer BAD NAME?")
  (right "A name cannot have a space in it")
  (wrong "Names must be in capitals"
    "You can type a name in small letters; BASIC turns it into capitals. The space is the trouble.")
  (wrong "GAME is a name BASIC uses itself"
    "GAME is not one of BASIC's names. The trouble is the space: a name is up to 8 letters and digits, with no spaces.")
  (hint "Look at what is between MY and GAME."))
```

So: `SAVE` keeps your program in a file, `DIR` lists the files, `LOAD`
brings one back, and F1 and `HELP` are there when you are stuck.
