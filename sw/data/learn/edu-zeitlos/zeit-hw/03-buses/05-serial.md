# Serial ports

```lesson
(objective "Know which serial port a program may use, wire a serial device the right way round, and open it from term.")
(minutes 15)
(keywords "UART1 (port)")
```

A Zeitlos computer has a UART for its serial console, UART0. The kernel
and the shell use it from the moment the board starts, so no program is
given it: changing its speed would cut the machine's last way in.

Some targets build a second one, **UART1**, for your own serial
devices. In the REPL, `uart1?` answers `#t` if this machine has one.

```sources
(source "Zeitlos docs: uart1.md, UART0 is not this, and never will be; Scheme API")
```

```check
(choice
  (ask "Why can a program not take the serial console's UART for its own device?")
  (right "It is the console: the kernel and its shell use it, and losing it can leave no way in")
  (wrong "It is too slow for devices" "Speed is not the reason. The console must stay the console, whatever else fails.")
  (wrong "It has no pins" "It has pins, and the console runs on them. That is exactly why it is kept for the console.")
  (hint "What is UART0 used for from the moment the board starts?"))
```

---

## Wiring: crossed

Each side sends on its TX and listens on its RX, so the board's TX goes
to the device's RX, its RX to the device's TX, and ground to ground.
Wiring TX to TX is the most common reason a serial link shows nothing.
Toggle the wiring.

```figure
(define crossed (toggle 'x "Cross TX and RX" #t))
(frame 20 50 90 110 'ink 1.5)
(text 65 40 "board" 'centre 'body 'bold)
(frame 210 50 90 110 'ink 1.5)
(text 255 40 "device" 'centre 'body 'bold)
(text 30 84 "TX" 'small)
(text 30 124 "RX" 'small)
(text 30 154 "GND" 'small)
(text 270 84 "TX" 'small)
(text 270 124 "RX" 'small)
(text 262 154 "GND" 'small)
(if crossed
    (begin (arrow 110 80 210 120 'ink 1.5) (arrow 210 80 110 120 'ink 1.5))
    (begin (line 110 80 210 80 'ink 1.5) (line 110 120 210 120 'ink 1.5)))
(line 110 150 210 150 'ink 1.5)
(text 160 200 (if crossed "each RX hears the other's TX" "TX meets TX: nobody hears anything") 'centre 'body 'bold)
```

```sources
(source "Timeless Computing (docs/tc.md), Talking to the World: UART")
```

```check
(choice
  (ask "A USB serial adapter's TXD is its output. Where does it go on the board?")
  (right "To the board's RX, the pin it receives on")
  (wrong "To the board's TX" "Two transmitters joined: both talk, neither listens. TX goes to RX.")
  (wrong "To GND" "Ground goes to ground. TXD carries the data, to the board's RX.")
  (hint "Every TX must meet a receiver."))
```

---

## Opening it

The program `serial` owns UART1 and lends it to `term`. In a `term`
window's REPL, type `serial 9600` for 9600 baud (`serial` alone keeps
the rate the port is at). F12 disconnects, back to term's panel. One window at a time
can use it.

Some rates cannot be made exactly from the board's clock. A rate more
than 3% off, such as 460800 or 921600, is refused rather than sent
wrongly; 9600, 115200 and 1000000 are fine.

```sources
(source "Zeitlos docs: uart1.md, Using it from term; Baud rates that don't exist")
```

```warn
(warn "No RS-232 ports" "An old computer's RS-232 serial port swings to plus and minus 12 V. Never wire it to a PMOD pin: use an adapter made to convert it to 3.3 V.")
```

```check
(choice
  (ask "You ask for 921600 baud and it is refused. Why?")
  (right "The board cannot make that rate closely enough, and a rate several percent off would garble every byte")
  (wrong "UART1 is busy with the console" "The console is UART0. The refusal is about the rate itself.")
  (wrong "No serial port can go that fast" "1000000 baud works exactly here. 921600 cannot be divided from the clock within 3%.")
  (hint "This lesson names the limit: 3%."))
```

---

```check
(choice
  (ask "In which order do you connect a 3.3 V serial device to UART1?")
  (right "Power off; TX to its RX, RX to its TX, GND to GND; power on; serial 9600 in the REPL")
  (wrong "Power on; serial 9600; then plug the wires in" "Connections are made with the power off.")
  (wrong "Power off; TX to TX, RX to RX, GND to GND; power on" "Like names do not meet: TX goes to the other side's RX.")
  (hint "Power off first, and cross the data lines."))
```

So: UART0 is the console and stays so; UART1, where a target builds it,
is yours, wired TX to RX and opened with `serial` from term.
