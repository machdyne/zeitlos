# Real pins on the bench

```lesson
(objective "Put a real GPIO port's pins and I2C bus on the bench, so that real and virtual parts work together.")
(minutes 20)
(keywords "segment (of a bus)")
```

A `gpio` part is a real PMOD port. Its pins, given by **PMOD pin
number** (1 to 4, 7 to 10), join bench nets:

```
gpio    g0  port=0  in=3  out=4
load    lamp  "Pin 3"  g0.3
switch  sw  "To pin 4"  g0.4  pullup
```

- An `in=` pin brings the real level onto its net, read every bench
  loop: a few hundredths of a second, fast enough for buttons and
  lamps, not for fast signals.
- An `out=` pin puts its net's level on the real pin, open drain: low
  pulls it down, anything else lets it go to the pull-up.

```sources
(source "Zeitlos docs: bench.md, Real hardware")
```

```check
(choice
  (ask "In gpio g0 port=0 in=3 out=4, which real PMOD pin does the lamp on g0.3 show?")
  (right "PMOD pin 3, which is bit 2 of port 0")
  (wrong "Bit 3, which is PMOD pin 4" "A gpio part counts PMOD pins, not bits. g0.3 is pin 3.")
  (wrong "Pin 7" "Pin 7 would be written g0.7. g0.3 is the connector's pin 3.")
  (hint "A gpio part's pins are numbered as on the connector."))
```

---

## Why open drain

Because an `out=` pin only ever pulls low or lets go, it is safe to
wire to a real pin that is itself an output: the two cannot fight with
one high and one low.

```check
(choice
  (ask "Why are a gpio part's out= pins open drain?")
  (right "So that one can be wired to a real pin that is itself an output, without a high and a low fighting")
  (wrong "So that they can switch motors and lamps" "Open drain gives no more current. It is about never driving high against something driving low.")
  (wrong "Because PMOD pins cannot be driven high" "They can: gpio-set drives a pin high. bench chooses not to, for safety.")
  (hint "What can two outputs on one wire do to each other?"))
```

---

## One bus, two worlds

`segment=` continues a bench bus onto a real port's pins 1 (SCL) and 2
(SDA): a transaction that no virtual part answers goes out on the real
wires. Real chips and modules then share one bus, a **segment** of
which is real.

```
bus   main  segment=g0
```

While bench owns a port, it is the port's only user. `i2c -b pmod0`
goes through bench rather than driving the same pins, and virtual time
stays at x1, because real hardware runs in real time.

```sources
(source "Zeitlos docs: bench.md, Real hardware; i2c.md, Buses by name: zi2cx")
```

```check
(choice
  (ask "bench owns port 0 with a gpio part. You type i2c scan, which uses pmod0. What drives the pins?")
  (right "bench: the i2c command's transactions go through it, so two programs never drive the same pins")
  (wrong "i2c, directly, at the same time as bench" "That would be two programs on one pair of pins. While bench owns the port, pmod0 goes through bench.")
  (wrong "Nothing: i2c refuses" "It works, through bench.")
  (hint "Who is the port's only user while the netlist is loaded?"))
```

---

```check
(choice
  (ask "With a real module on the bench, you press 3 for x3600. What happens?")
  (right "Time stays at x1: real hardware runs in real time")
  (wrong "The module runs 3600 times faster" "bench cannot speed up real hardware, so it keeps time at x1 and says so.")
  (wrong "The netlist is reloaded" "That is F5. 3 asks for x3600, which bench refuses while real hardware is on it.")
  (hint "Can bench change how fast a real chip runs?"))
```

So: a `gpio` part brings real pins onto bench nets, by PMOD pin
number, and `segment=` joins a bench bus to the real I2C wires.
