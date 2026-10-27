# A module's program

```lesson
(objective "Stop, start and send a program to a Sechs module, type into its console, read its registers and change its address.")
(minutes 25)
```

A module runs a BASIC program. From the master you can stop it, start
it and start the module again:

| Command | Does |
|---|---|
| `sechs halt ADDR` | stops its program |
| `sechs run ADDR` | starts it |
| `sechs reset ADDR` | restarts the module |
| `sechs send ADDR FILE` | types the file into the module, a line at a time |

So a program goes in with `send` and starts with `run`. The example
`/data/basic/BLINK.BAS` blinks a module's pin C:

```
$ sechs send 0x0c /data/basic/BLINK.BAS
$ sechs run 0x0c
```

```sources
(source "Zeitlos docs: sechs_app.md, Commands; bench.md, Real hardware")
```

```check
(choice
  (ask "You sent a new program to the module at 0x0c. Which command starts it?")
  (right "sechs run 0x0c")
  (wrong "sechs send 0x0c again" "send types the program in. run starts it.")
  (wrong "sechs info 0x0c" "info shows the module's identity and state; it starts nothing.")
  (hint "Halt, run, reset: which one starts?"))
```

---

## The console

`sechs console ADDR` lets you type straight into the module's BASIC, as
if at its keyboard. This is its **console**: you type a line, press
Enter, and its answer appears; a running program's output appears too.
Ctrl-D ends the session and gives the terminal back to the shell.

```
$ sechs -b main console 0x0c
console on 0x0c; end with Ctrl-D
10 PRINT "HELLO"
RUN
HELLO
```

```sources
(source "Zeitlos docs: sechs_app.md, The console")
```

```check
(choice
  (ask "You are in sechs console and want the shell back. What do you press?")
  (right "Ctrl-D")
  (wrong "F12" "F12 disconnects the whole term window from the shell. Ctrl-D ends just the console session.")
  (wrong "Esc" "Esc is not how the console ends. It says so when it opens: end with Ctrl-D.")
  (hint "The console's first line says how to end it."))
```

---

## Registers

A module has sixteen program registers, 0 to 15, each a byte, which
its program and the master share: the program writes a reading there,
the master reads it, or the master writes a setting. In BASIC they are
`REG`. In `modpanel.net`, the module puts its buttons in register 0:

```
$ sechs -b main reg 0x0c 0
```

`sechs reg 0x0c 2 100` writes 100 into register 2.

```sources
(source "Zeitlos docs: sechs_app.md, Commands; sw/apps/bench/examples/modpanel.net")
```

```check
(text
  (ask "Type the command that writes 50 into register 1 of the module at 0x0c, on PMOD port 0.")
  (accept "sechs reg 0x0c 1 50") (accept "sechs reg 12 1 50") (accept "sechs -p 0 reg 0x0c 1 50")
  (wrong-if "sechs reg 0x0c 50 1" "The register comes first, then the value.")
  (wrong-if "sechs reg 0x0c 1" "That reads register 1. Add the value to write it.")
  (hint "sechs reg ADDR N VALUE"))
```

---

## Several modules

Every new module answers at 0x0c, so before a second one joins the
bus, give the first a new address: `sechs addr 0x0c 0x0d`. The module
keeps it. Then plug in the next one.

```warn
(warn "One module at a time" "Plug each new module in with the power unplugged, and change its address before the next one goes on the bus: two modules at one address both answer at once.")
```

```check
(choice
  (ask "You have two new modules for one bus. What do you do?")
  (right "Connect one, give it a new address with sechs addr 0x0c 0x0d, then connect the second")
  (wrong "Connect both, then sechs addr 0x0c 0x0d" "Both would answer at 0x0c, and both would take the new address. Change the first before the second joins.")
  (wrong "Nothing: the bus sorts out two modules at one address" "I2C has no way to tell two devices at one address apart.")
  (hint "A new module always answers at 0x0c."))
```

---

## What sechs cannot do

`sechs` changes a module's programs, not its firmware, Machdyne BASIC
itself. Writing firmware needs pulses about 80 ns long, which software
on this machine cannot time; Machdyne's Werkzeug tool does it.

```sources
(source "Zeitlos docs: sechs_app.md, What it does not do")
```

```check
(choice
  (ask "You want to put a new version of Machdyne BASIC into a module. Can sechs do it?")
  (right "No: firmware is written with a programmer such as Werkzeug")
  (wrong "Yes, with sechs send" "send types BASIC lines into the running firmware. It cannot replace the firmware.")
  (wrong "Yes, with sechs program" "program restarts a module in its programming mode, if it has one. Writing the firmware needs timing that software here cannot give.")
  (hint "Programs are text; firmware is what runs them."))
```

So: `send` and `run` load and start a program, `console` types into
the module, `reg` reads and writes its registers, and `addr` gives each
module its own address.
