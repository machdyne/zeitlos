# Trying a design on the board

```lesson
(objective "Choose a safe way to load a bitstream you built, and get back to Zeitlos afterwards.")
(minutes 20)
(keywords "jumploader (FPGA)")
```

Building a bitstream changes nothing on the machine: it is just a file.
Loading it is different. The ECP5 is configured whole, so while your
design runs, **Zeitlos is not running at all**: no desktop, no files,
no keyboard. The FPGA is your counter and nothing else, until it is
loaded again.

```sources
(source "docs/zfpga.md, section 1.1")
```

```check
(choice
  (ask "Your blinky is running on the FPGA. Can you switch back to the desktop to check something?")
  (right "No: the FPGA is only your design now, and Zeitlos returns when the FPGA is loaded again")
  (wrong "Yes, the desktop keeps running beside it"
    "The ECP5 loads a whole configuration. Your design replaced the processor and everything else, so nothing of Zeitlos is left running.")
  (hint "How much of the FPGA does a bitstream set?"))
```

---

```warn
(warn "Never write over Zeitlos's own MMOD" "Write your designs only to a spare module. Keep the MMOD that holds Zeitlos out of the socket while you try your own, and put it back to return. A design written over Zeitlos's module leaves a machine that starts only your design, until it is written again from another computer.")
```

## A spare MMOD

On a board whose configuration flash is a removable MMOD, such as the
Lakritz, the safe way needs no other computer:

1. Put a spare MMOD in a Pmod socket and open the `mmod` app.
2. Choose the socket and press **DETECT**; it must find the module.
3. Erase from 0, then open your `.bit` and **WRITE** it, then
   **VERIFY**.
4. Power off. Take Zeitlos's MMOD out of the configuration socket and
   put the spare in. Power on: your design starts.
5. To return: power off, put Zeitlos's MMOD back, power on.

[docs:zfpga-test](docs:zfpga-test) gives every step for a Lakritz.

```sources
(source "docs/zfpga-test.md, sections 3 and 4; docs/mmod.md")
```

```check
(choice
  (ask "Why is the bitstream written at address 0 of the spare MMOD?")
  (right "Power-on reads address 0, and there is no bootloader on the spare")
  (wrong "Because zfpga only writes to address 0"
    "The mmod app writes it, from the START you give. It goes at 0 because that is what the FPGA reads at power-on.")
  (wrong "So that it does not overwrite Zeitlos"
    "Zeitlos is on the other module, which is never written. Address 0 is simply where power-on reads.")
  (hint "Where does power-on always read?"))
```

---

## With another computer and a cable

A computer with openFPGALoader can load a bitstream straight into the
FPGA's configuration memory over a JTAG cable, with no flash written
at all. It lasts until the power is switched off; then the board
starts Zeitlos from its flash as before.

```sources
(source "docs/zfpga-test.md, section 5; Timeless Computing (docs/tc.md), The Machine: Getting Back to Safety")
```

```check
(choice
  (ask "You load a design over a cable into the configuration memory. How do you get Zeitlos back?")
  (right "Switch the board off and on: nothing in the flash was changed")
  (wrong "Reflash Zeitlos from the other computer"
    "Loading into configuration memory writes no flash, so Zeitlos is still there. A power cycle loads it.")
  (hint "Is configuration memory kept without power?"))
```

---

## zfpga flash and zfpga run

zfpga can also write a bitstream into this machine's flash, in
free space, never over the bootloader or Zeitlos:
`zfpga flash /home/slow.bit` writes it and checks it.
`zfpga run /home/slow.bit` writes it and then boots it.

`run` boots through a **jumploader**, a tiny bitstream that Zeitlos
reloads into and that sends the FPGA on to your design. Only gateware
built with `JUMP=1`, on a flash larger than 2 MB, has one; release
images do not. Without it, `run` refuses and says why, and `flash` can
store a design but not start it.

Whichever way you load a design, switching off and on with Zeitlos's
own module in place brings Zeitlos back.

```sources
(source "docs/zfpga-formats.md, section 11; docs/zboot.md, section 5 (the layout); sw/apps/zfpga/boot.c")
```

```check
(choice
  (ask "On a machine installed from a release image, you type zfpga run /home/slow.bit. What happens?")
  (right "It refuses, because there is no jumploader to boot through")
  (wrong "The design starts, and Zeitlos stops"
    "run needs a jumploader, and release images do not carry one. zfpga checks first and refuses, saying so.")
  (wrong "Zeitlos's gateware is overwritten"
    "zfpga writes only to free space, never over Zeitlos. And here run refuses before writing anything.")
  (hint "What does run boot through?"))
```

So: a design replaces Zeitlos while it runs, so try it from a spare
MMOD or over a cable, and a power cycle with Zeitlos's module in place
brings Zeitlos back.
