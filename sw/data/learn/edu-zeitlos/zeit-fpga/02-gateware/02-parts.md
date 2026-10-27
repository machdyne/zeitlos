# The parts and their addresses

```lesson
(objective "Match parts of the SOC to their files and to their places in the address map.")
(minutes 20)
(keywords "chip select (FPGA)" "Wishbone (bus)" "BIOS (boot program)" "CSRs (control registers)")
```

The parts of the SOC talk over one bus, **Wishbone**: the processor,
and other masters such as the blitter, put an address on it, and one
device answers. An arbiter, `rtl/arbiter_main.v`, decides who has the
bus when two want it.

Which device answers is decided by a line like this in `sysctl.v`:

```verilog
wire cs_gpio = ((wbm_adr & 32'hf000_0000) == 32'he000_0000);
```

`cs_gpio` is a **chip select**: a signal that is 1 when the address
belongs to that device. The mask keeps the top 4 bits of the address;
if they are E, the GPIO block answers.

```sources
(source "rtl/sysctl.v, the cs_ wires; Timeless Computing (docs/tc.md), Systems on a Chip: Adding a Device, End to End")
```

```check
(choice
  (ask "The processor reads address 0xE000_0004. Which chip select is 1?")
  (right "cs_gpio")
  (wrong "None: the address is not exactly 0xE000_0000"
    "The mask keeps only the top 4 bits. Every address from 0xE000_0000 to 0xEFFF_FFFF has E there.")
  (hint "Look only at the top hex digit."))
```

---

## The map

The top hex digit of an address picks the part. The **BIOS** is the
boot program, held in block RAM; the control registers, or **CSRs**,
say what the gateware has and control the SOC.

| Address | Part | Source |
|---|---|---|
| 0x0000_0000 | the BIOS, in block RAM | rtl/mem/bram.v |
| 0x2000_0000 | video memory | |
| 0x4000_0000 | main memory: SDRAM or DDR3 | rtl/mem/ |
| 0x7000_0000 | the CSRs | rtl/csrs.v, rtl/socctl.v |
| 0xA000_0000 | graphics | rtl/gpu/ |
| 0xD000_0000 | the blitter | rtl/gpu/gpu_blit.v |
| 0xE000_0000 | GPIO: LEDs and Pmod pins | rtl/gpio.v |
| 0xF000_0000 | the serial port, UART0 | rtl/uart.v |

```sources
(source "rtl/sysctl.v, the chip selects (cs_bram, cs_sdram, cs_ddr3, cs_vram, cs_csrs, cs_gpu, cs_gpu_blit, cs_gpio, cs_uart0)")
```

```check
(choice
  (ask "A program writes a character to 0xF000_0000. Where does it go?")
  (right "Out of the serial port, UART0")
  (wrong "To the screen, through the graphics"
    "The graphics answer at 0xA. F is the serial port.")
  (wrong "Into main memory"
    "Main memory is at 0x4. Writes to 0xF reach the UART.")
  (hint "Find F in the table."))
```

---

```check
(text
  (ask "Which file holds the Verilog for the device that answers at 0xE000_0000? Give its path.")
  (accept "rtl/gpio.v") (accept "gpio.v") (accept "/rtl/gpio.v")
  (wrong-if "rtl/uart.v" "The UART is at 0xF. E is the GPIO, which owns the LEDs.")
  (hint "E is GPIO, in the rtl folder."))
```

---

## Masters and devices

Not only the processor uses the bus. The blitter reads the pixels
of the rectangles it copies, and the audio fetches its samples from
memory. Each of these is a master, which is why an arbiter is needed.

```check
(choice
  (ask "Why does the SOC need an arbiter on its bus?")
  (right "Because more than one master, such as the processor and the blitter, can ask for the bus at once")
  (wrong "Because the address decoder is too slow"
    "The decoder picks a device for an address. The arbiter picks which master's address goes on the bus first.")
  (wrong "To decide which device answers"
    "That is the chip selects' job. The arbiter chooses between masters, not devices.")
  (hint "Masters ask; devices answer."))
```

So: in the gateware, a chip select decodes each address's top digit to
one part, and an arbiter shares the bus between masters.
