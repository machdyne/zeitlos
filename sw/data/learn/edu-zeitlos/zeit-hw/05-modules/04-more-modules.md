# More modules, and any device

```lesson
(objective "Say which Machdyne modules need a target built for them and which use a GPIO port, and add any new device in a safe order.")
(minutes 20)
```

Some modules need the FPGA's design to have their own hardware in it,
so they work only with a target built for them:

| Module | What it is | A target for it |
|---|---|---|
| Langkatze | SPI ethernet (an ENC28J60 chip) | `lakritz_langkatze`, and in A on `obst_langkatze_gpio` |
| Katze | RMII ethernet (only the chip that drives the cable) | `lakritz_katze` |
| Kabellos | WiFi (an ESP32-C3 module) | `lakritz_kabellos` |

Others need only a GPIO port: Sechs modules on a Wolfszahn, and MMOD
memory modules, read with the `mmod` app.

```sources
(source "Zeitlos docs: networking.md (Langkatze); katze.md; kabellos.md; releases.md; mmod.md; sechs_app.md")
```

```check
(choice
  (ask "You plug a Kabellos into a machine running minze_gpio. Does its WiFi work?")
  (right "No: WiFi through Kabellos needs a target built for it, such as lakritz_kabellos")
  (wrong "Yes: any module works in a GPIO port" "Sechs modules and MMODs work over GPIO pins. Kabellos needs its own link built into the FPGA's design.")
  (wrong "Yes, after i2c scan finds it" "Kabellos is not an I2C device. It needs a target that builds its link.")
  (hint "Which of the modules work with just a GPIO port?"))
```

---

## Adding any device

Whatever you connect, the same order keeps it safe:

1. **Read the data sheet**: its supply voltage, its bus and address,
   whether it needs pull-ups, and which register holds what.
2. **Check** that it works at 3.3 V and that the bus is one the board
   can speak.
3. **Connect it with the power off**: supply, ground and signals, then
   the checks: shared ground, no short.
4. **Confirm it answers**: `i2c scan` for an I2C device.
5. Only then **write the program** that uses it.

```sources
(source "Timeless Computing (docs/tc.md), Talking to the World: Connecting a Device")
```

```check
(choice
  (ask "A new I2C sensor is wired, and the power is on. What comes next?")
  (right "i2c scan, to confirm it answers at its address")
  (wrong "Write the program that reads it" "First make sure it answers. A program cannot tell you whether the wiring is wrong.")
  (wrong "Read its data sheet" "That came first, before wiring: the voltage, the bus and the address.")
  (hint "Step 4 of the five."))
```

---

## Practice

```example
(example
  (problem "Connect an LM75 temperature sensor to port 0 and read it.")
  (step "Its data sheet: 3 to 5.5 V supply, so 3V3 is fine; I2C at 0x48 with its address pins at ground.")
  (step "Power off. Its SCL to PMOD pin 1, SDA to pin 2, VCC to 3V3, GND to GND.")
  (step "Power on. In POSIX, i2c scan prints 0x48.")
  (step (choice (ask "Which command reads its temperature?")
          (right "i2c write 0x48 0 read 1")
          (wrong "i2c read 0x48 0" "read takes a count, not a register. Write the register number first, then read.")
          (wrong "sechs info 0x48" "The LM75 is not a Sechs module. It is read with i2c.")))
  (answer "i2c write 0x48 0 read 1 prints the temperature in whole degrees, in hex."))
```

```check
(choice
  (ask "Your scan finds nothing at 0x48. In which order do you look?")
  (right "Pull-ups, then SCL and SDA swapped, then power, then the address")
  (wrong "The address first, then power" "The usual causes come in a known order: pull-ups and swapped lines before power and address.")
  (wrong "Rewrite the program first" "No program is involved in a scan. The cause is in the wiring.")
  (hint "The order from the lesson on when nothing answers."))
```

---

```check
(choice (several)
  (ask "Which of these work with nothing more than a GPIO port? Choose all that do.")
  (right "A Sechs module in a Wolfszahn")
  (right "An MMOD memory module, with the mmod app")
  (wrong "A Langkatze ethernet module" "Langkatze needs a target that builds its SPI ethernet link, such as lakritz_langkatze.")
  (wrong "A Katze ethernet module" "Katze carries only the chip that drives the cable: its ethernet controller must be in the FPGA's design, in lakritz_katze.")
  (hint "Two of them are reached by software on ordinary pins."))
```

So: some modules need a target of their own, Sechs modules and MMODs
need only GPIO pins, and every device goes on with its data sheet read,
the power off, and a scan before the program.
