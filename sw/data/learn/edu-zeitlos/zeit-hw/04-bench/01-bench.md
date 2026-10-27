# The bench

```lesson
(objective "Open a netlist in bench and read its cards: which pins are high, low, floating or in conflict.")
(minutes 20)
(keywords "bench (the app)" "netlist" "card (on the bench)" "net (on the bench)" "status line (of bench)")
```

**bench** is an app where virtual parts live: I/O chips, LEDs, buttons,
switches, lamps. Pins joined together make a **net**, and a
**netlist**, a short text file ending in `.net`, says which parts there
are and how their pins are joined. Programs reach the
parts' I2C buses as they would a real bus, so you can try a circuit and
its program before anything is wired, on any Zeitlos computer.

bench is not on the dock. Open `files` from the dock, go to the folder
`/data/bench/examples`, and open `panel.net`: bench starts with it.

```sources
(source "Zeitlos docs: bench.md; sw/common/ztype.c (.net files open in bench)")
```

```try
(try (app "files") (task "Open files from the dock, go to /data/bench/examples and open panel.net. Leave bench open and come back."))
```

```check
(choice
  (ask "What does a netlist hold?")
  (right "The parts on the bench and how their pins are joined")
  (wrong "The program that drives the parts" "Programs are separate: a shell command, a BASIC program, a module's program. The netlist only describes the circuit.")
  (wrong "A recording of a real circuit's signals" "It is a description you write, as plain text, of parts and their connections.")
  (hint "It is a list of parts and their nets."))
```

---

## Cards

bench shows one **card** for each part, with a square for each of its
pins:

```figure
(frame 40 30 24 24 'ink 1.5)
(rect 43 33 18 18 'ink)
(text 80 48 "high" 'body)
(frame 40 70 24 24 'ink 1.5)
(text 80 88 "low" 'body)
(frame 40 110 24 24 'grey 1)
(text 52 128 "?" 'centre 'body)
(text 80 128 "floating: nothing drives it" 'small)
(frame 40 150 24 24 'ink 1.5)
(line 40 150 64 174 'ink 1.5)
(line 64 150 40 174 'ink 1.5)
(text 80 168 "conflict: two outputs disagree" 'small)
(caption "How bench draws a pin")
```

Under a pin, `o` means the part drives it, `i` that it does not. An LED
or a lamp is filled when lit.

```check
(choice
  (ask "On an I/O chip's card, a pin shows a dotted square with a ?. What does it mean?")
  (right "Nothing drives that pin, and nothing pulls it: it floats")
  (wrong "The pin is low" "A low pin is an empty square. The ? means bench cannot give it a level: nothing drives or pulls it.")
  (wrong "Two outputs disagree on it" "That is a square with an X: a conflict.")
  (hint "The ? stands for a level nobody sets."))
```

---

## Nets and the status line

Click a pin, and the
**status line** at the top of the window shows its net: its level, any
pull-up or pull-down on it, and every pin on it. Otherwise the status
line shows the netlist, the virtual time and its speed, the parts, and
how many conflicts there have been: a conflict is never silent.

```check
(choice
  (ask "An LED does not light, though you think its pin is high. What do you click to see why?")
  (right "The LED's pin: the status line shows its net and every pin on it")
  (wrong "The LED's card twice" "Clicking a button or switch presses it. To see a net, click a pin.")
  (wrong "Nothing: the status line only shows the time" "Clicking a pin puts its net in the status line. Esc brings the usual line back.")
  (hint "What does clicking a pin show?"))
```

---

## Keys

| Key | Does |
|---|---|
| F1 | help |
| F5 | read the netlist again, after you edit it |
| F6 | the cards, then the bus log, then the modules' consoles |
| 1, 2, 3 | virtual time at x1, x60, x3600 |
| Space | pause virtual time |
| Esc | the status line back |

Clicking a button presses it while you hold it; clicking a switch
flips it. The open icon in the title bar loads another netlist.

```sources
(source "Zeitlos docs: bench.md, The screen; sw/apps/bench/bench_app.c (keys and help)")
```

```check
(choice
  (ask "You changed panel.net in a text editor. What makes bench use the new version?")
  (right "F5")
  (wrong "F6" "F6 switches between the cards, the bus log and the consoles. F5 reads the netlist again.")
  (wrong "Space" "Space pauses virtual time.")
  (wrong "Close bench and the change is lost" "Your edit is in the file. F5 reads it again without closing.")
  (hint "This key reads the file again."))
```

---

```check
(choice
  (ask "Every transaction on bench's buses can be seen, newest last. Where?")
  (right "In the bus log: press F6")
  (wrong "In the status line" "The status line shows a net or a summary. The transactions are in the bus log, one F6 away.")
  (wrong "Nowhere: buses are not shown" "bench keeps a log of every transaction. F6 shows it.")
  (hint "F6 goes round three views."))
```

So: bench shows a netlist's parts as cards, a pin as high, low,
floating or in conflict, and a net in its status line.
