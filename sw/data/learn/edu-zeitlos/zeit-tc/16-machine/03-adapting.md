# Adapting it, and getting back

```lesson
(objective "Choose the right level at which to adapt the machine, and get it back to a working state when a change goes wrong.")
(minutes 20)
(keywords "rescue console" "release image")
```

You will see the six levels at which the machine can be changed, and
the ways back when a change goes wrong.

1. **Settings**: screen, keyboard layout, time zone, the services that
   start. Most can be changed with the settings program.
2. **Scripts and schedules**: a few lines of Scheme rename files,
   process text or fetch data; a scheduler runs programs at set times.
3. **Programs**: a C program compiled on another computer and put in
   `/apps` becomes an application like the others.
4. **Hardware**: the Pmod sockets give pins, I2C and SPI to Scheme and C,
   for sensors, displays, relays and Sechs modules.
5. **The gateware**: the SOC is open Verilog; it can be changed and
   rebuilt with the open FPGA tools on another computer.
6. **The system**: the source of every program and of the kernel is
   open.

Each level is deeper, and harder, than the one before.

```sources
(source "Timeless Computing (docs/tc.md), The Machine: Adapting the Machine")
```

```check
(choice
  (ask "You want the machine to start with a different keyboard layout. Which is the right level?")
  (right "Settings")
  (wrong "The system: change the kernel"
    "The keyboard layout is a line in the configuration file, changed with the settings program. No source needs changing.")
  (wrong "Programs: write a new keyboard driver"
    "A layout is a setting. Start at the shallowest level that does the job.")
  (hint "Start at the shallowest level that does the job."))
```

---

## An example

To log a room's temperature every hour:

1. **The device.** A temperature sensor goes on an I2C pin pair of a
   Pmod socket. A scan of the bus shows it answers.
2. **A trial.** In the Scheme terminal, the I2C procedures read one
   byte from it, checked against a thermometer.
3. **A script.** A short Scheme file reads the sensor and adds a line
   with the date, time and temperature to a text file in `/home`.
4. **A schedule.** Run every hour, it builds a plain text record any
   program can read.
5. **A record.** The script, the schedule and a note of the wiring are
   kept, and copied, with the owner's other files.

```check
(choice (several)
  (ask "Which two levels does this example use?")
  (right "Hardware")
  (right "Scripts and schedules")
  (wrong "The gateware"
    "The SOC is unchanged: the sensor uses the I2C pins it already has.")
  (wrong "The system"
    "No program or kernel is rebuilt. A Scheme script and a schedule do it.")
  (hint "A sensor is wired, and a short Scheme file is run every hour."))
```

```sources
(source "Timeless Computing (docs/tc.md), The Machine: An Example Adaptation")
```

---

## The ways back

- **The core is in flash**, so the machine starts without a card. A
  card with a faulty program on it can be taken out.
- **Card copies win over flash copies.** Deleting the copy in `/apps`
  brings back the original.
- **Crashes are contained**: a faulty program is stopped and reported;
  a faulty driver is restarted, up to a few times a minute.
- **A design loaded straight into the FPGA is not kept.** It lasts only
  until the power is turned off; then the FPGA loads its working
  design from flash again.

```sources
(source "Timeless Computing (docs/tc.md), The Machine: Getting Back to Safety")
(source "Zeitlos documentation: flash_apps (docs/flash_apps.md)")
```

```check
(choice
  (ask "You put a new version of the file browser in the card's /apps, and it crashes at once. How do you get the old one back?")
  (right "Delete the copy in /apps, so the flash copy runs again")
  (wrong "Rewrite the whole flash from another computer"
    "That works, but it is far more than needed. The flash copy was never changed: delete the card's copy.")
  (wrong "It cannot be undone"
    "The original is still in flash. Only the card's copy is in the way.")
  (hint "Which copy wins, and is the other one still there?"))
```

---

## The rescue console

If the desktop cannot start, a **rescue console** appears: the kernel's
own shell on the monitor, typed at on the USB keyboard. It can also be
called up by holding Esc while the machine starts, until the console
appears. From it, programs can be started by hand and problems looked into.

```sources
(source "Zeitlos documentation: rescue_console (docs/rescue_console.md)")
```

```check
(choice
  (ask "After a change, the desktop never appears. What do you reach for?")
  (right "The rescue console, which comes up by itself or with Esc held while the machine starts")
  (wrong "Nothing can be done without another computer"
    "The rescue console runs on the machine itself, on its monitor and USB keyboard.")
  (wrong "F1, the help key"
    "F1 shows an app's help on the desktop. With no desktop, the rescue console is the way in.")
  (hint "Which tool works with no desktop at all?"))
```

---

## Spares and images

A **second MMOD** is the safest place to try a new design: fit it for
the experiment, and fit the original back afterwards. The machine can
copy its own configuration module to a spare as a backup, with the
`mmod` program.

A **release image** puts the whole machine back as it first was; it is
written to the flash with the open tools from another computer.

With a spare MMOD holding a known good system, and a copy of the
release image, almost any change can be undone in a few minutes.

```sources
(source "Timeless Computing (docs/tc.md), The Machine: Getting Back to Safety")
(source "Zeitlos documentation: mmod (docs/mmod.md)")
```

```check
(choice
  (ask "You have built new gateware and want to try it safely. What is the safest way?")
  (right "Load it straight into the FPGA first, then write it to a spare MMOD, keeping the original")
  (wrong "Write it over the MMOD's working design at once"
    "If it fails, the machine no longer starts its working design. Try it in a way that keeps the original.")
  (wrong "Put it in the card's /apps folder"
    "/apps holds programs, not FPGA designs. Gateware lives in the configuration flash, on the MMOD.")
  (hint "Which ways keep the working design untouched?"))
```

So: adapt the machine at the shallowest level that does the job, and
keep the ways back ready: flash, the card, the rescue console, a spare
MMOD and a release image.
