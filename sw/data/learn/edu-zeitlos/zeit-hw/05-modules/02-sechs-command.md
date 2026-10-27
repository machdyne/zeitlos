# Finding modules

```lesson
(objective "Find Sechs modules with sechs scan and identify one with sechs info, on a PMOD port or on the bench.")
(minutes 20)
```

The `sechs` command, in the POSIX shell, is a Sechs master. It finds
the modules on a bus and says what state each is in:

```
$ sechs scan
0x0c running networked
```

Each line is a module's address and its status. A new module answers
at 0x0c; with none, `sechs scan` says `no modules found`.

By default `sechs` uses PMOD port 0, A on pin 1 and B on pin 2. `-p 1`
chooses port 1, and `-b main` a bench bus.

```sources
(source "Zeitlos docs: sechs_app.md; ls99.md, Settings (0x0c for a new module)")
```

```check
(choice
  (ask "sechs scan prints no modules found, with a module in a Wolfszahn on port 1. What did you forget?")
  (right "-p 1: sechs uses port 0 unless told otherwise")
  (wrong "To start the module's program first" "A halted module still answers a scan. The scan looked on port 0, and the module is on port 1.")
  (wrong "To give the module an address" "A new module already answers at 0x0c. The scan was on the wrong port.")
  (hint "Which port does sechs use by default?"))
```

---

## A module on the bench

You can practise with no module at all. `growlight.net`, in
`/data/bench/examples`, puts a virtual LS10 on the bench bus `main`,
running a program that times a grow light. Open it from `files`, then
in the POSIX shell:

```
$ sechs -b main scan
$ sechs -b main info 0x0c
```

A virtual module answers as the real one does.

```sources
(source "Zeitlos docs: bench.md, Examples; ls99.md; sw/apps/bench/examples/growlight.net")
```

```try
(try (app "term") (task "Open growlight.net in bench (from files, in /data/bench/examples). Then in term, choose POSIX and type sechs -b main info 0x0c. Then come back."))
```

```check
(text
  (ask "Type the command that shows the identity of the module at 0x0c on the bench bus main.")
  (accept "sechs -b main info 0x0c") (accept "sechs -b main info 12")
  (accept "sechs -b main info 0x0C")
  (wrong-if "sechs info 0x0c" "Without -b main, sechs looks on PMOD port 0, not on the bench.")
  (wrong-if "sechs -b main scan" "scan lists every module. info shows one module's identity.")
  (hint "sechs, the bus, info, the address."))
```

---

## What info tells you

```
address 0x0c
version 0.5
status  running networked
fw=Machdyne BASIC
mod=LS10A
lang=basic
```

The last lines are the module's own description: its firmware, the
kind of module and its language. A virtual module says `mod=LS99`,
which is never a real module, and `profile=LS10` for the module it
plays.

```sources
(source "Zeitlos docs: sechs_app.md; ls99.md, Settings")
```

```check
(choice
  (ask "sechs info shows mod=LS99. What is at that address?")
  (right "A virtual module, on a bench bus")
  (wrong "A real LS99 module" "LS99 is never a real module: it is the bench's virtual one.")
  (wrong "A broken LS10" "A module reports what it is. LS99 means virtual.")
  (hint "Which module exists only on the bench?"))
```

---

```check
(number
  (ask "A new module answers at 0x0c. What is that in decimal?")
  (answer 12) (from "0 * 16 + 12")
  (wrong-if 0 "c is a hex digit, twelve.")
  (hint "c is twelve."))
```

So: `sechs scan` lists the modules on a bus, `sechs info` identifies
one, and `-p` or `-b` chooses the bus.
