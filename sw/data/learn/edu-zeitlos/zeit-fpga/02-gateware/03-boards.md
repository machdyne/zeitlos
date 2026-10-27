# One design, many boards

```lesson
(objective "Say what changes from one board's gateware to another's, and where each board's choices are kept.")
(minutes 15)
(keywords "feature bit (of the gateware)")
```

The same `rtl` folder builds the gateware for every board. What
differs is kept in two places.

The first is `rtl/boards.vh`. For each board it has a block of
defines, which switch parts of the Verilog on or off: the source tests
them with `` `ifdef``. Part of the Lakritz's block:

```verilog
`elsif BOARD_LAKRITZ
`define FPGA_ECP5
`define PROGRAMN_PIN
`define MEM 32
`define MEM_SDRAM
`define GPU
`define COLOR
`define USB_HOST
```

So the Lakritz gets 32 MB of SDRAM, graphics, colour and a USB host.
On the Lakritz, some cryptography blocks are left out, commented out
in this block, to make room for colour.

```sources
(source "rtl/boards.vh, the BOARD_LAKRITZ block")
```

```check
(choice
  (ask "You want a Lakritz build without colour. What do you change?")
  (right "Remove `define COLOR from the Lakritz block of rtl/boards.vh, then build again")
  (wrong "Delete the colour circuits' Verilog files"
    "Other boards still use them. A board leaves a part out by not defining its name; the files stay.")
  (wrong "Turn colour off in settings"
    "That changes what the software does with the hardware. To change the hardware, the gateware itself is built without it.")
  (hint "Each board's parts are switched on by defines."))
```

---

## The constraints file

The second place is the board's constraints file, in the folder
`boards` of the source: `boards/lakritz_v0.lpf`, `boards/mozart_ml2.lpf`
and so on. It says which pin of the FPGA each port of the top module
is on, and with what voltage standard.

The processor, the bus, the address map and every program are the
same on every board. The pins, the memory, the clocks and how much
fits are what change.

```sources
(source "boards/*.lpf; Timeless Computing (docs/tc.md), Systems on a Chip: Moving a Design to Another Board")
```

```check
(choice (several)
  (ask "Which of these differ between a Lakritz's gateware and a Mozart ML2's? Choose every one.")
  (right "Which FPGA pin carries each signal")
  (right "The kind of main memory and its controller")
  (wrong "The address of the serial port"
    "The address map is the same on every board: the UART is at 0xF000_0000 on all of them, so the same programs run.")
  (wrong "The instructions the processor runs"
    "Every board has the same RV32IM processor, so one kernel and one set of apps serve them all.")
  (hint "Think of what is physically different about the boards."))
```

---

## Seeing which parts your gateware has

The running gateware reports what it was built with: **feature bits** in
its control registers at 0x7000_0000. The `info` app shows them, along
with memory and the processes running. Open it to see which parts your
board's gateware includes.

```sources
(source "docs/info_app.md; docs/csrs.md")
```

```check
(choice
  (ask "Where does the running system learn which optional parts its gateware has?")
  (right "From feature bits the gateware itself reports, in its control registers")
  (wrong "From rtl/boards.vh on the card"
    "The source is not on the machine at all. The gateware answers for itself, through registers built into it.")
  (wrong "It cannot know; it tries each device"
    "Trying a device that is not there can stall the bus. The control registers say which are present.")
  (hint "The control registers are at 0x7000_0000."))
```

So: one Verilog source serves every board; boards.vh chooses each
board's parts, and its constraints file places them on its pins.
