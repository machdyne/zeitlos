# The shape of an SOC

```lesson
(objective "Name the parts of an FPGA computer, and follow one write from a program to a light on a pin.")
(minutes 25)
(keywords "arbiter" "address decoder" "boot ROM" "memory controller")
```

You will see how an FPGA computer is put together, then follow one
instruction all the way to a light.

```text
  +-------+   +--------+   +-----+          (bus masters)
  |  CPU  |   | video  |   | DMA |
  +---+---+   +---+----+   +--+--+
      |           |           |
  +---+-----------+-----------+-----------------------+
  |      bus, with an arbiter and an address decoder  |
  +---+--------+--------+--------+--------+--------+--+
      |        |        |        |        |        |
   boot ROM  memory    UART     USB     pins    audio
             ctrl
               |
          external RAM
```

The CPU is one of several **bus masters**: parts that start transfers
on the bus. The video circuit, which reads the screen's pixels from
memory, is another, and so is DMA, which copies data without the CPU.
When two ask for the bus at once, the **arbiter** decides which one
gets it.

```sources
(source "Timeless Computing (docs/tc.md), Systems on a Chip: The Shape of an SOC")
```

```check
(choice
  (ask "The CPU and the video circuit both want the bus in the same cycle. What decides?")
  (right "The arbiter")
  (wrong "The UART"
    "The UART is a device on the bus, which masters talk to. Choosing between two masters is the arbiter's job.")
  (wrong "The CPU always wins"
    "If the CPU always won, the video circuit could miss the pixels it needs for the screen. The arbiter decides between them.")
  (hint "Which part chooses between bus masters?"))
```

---

## The other parts

- The **address decoder** turns each address into a select signal for
  one device.
- The **boot ROM** is a small program in block RAM, built into the
  bitstream, so the processor has something to run as soon as the FPGA
  is configured. It loads the operating system from flash.
- The **memory controller** joins the bus to the external RAM.
- Each of the other devices shows a few registers on the bus.

```check
(choice
  (ask "The FPGA has just loaded its bitstream. Where is the first program the processor runs?")
  (right "In the boot ROM, which is part of the bitstream")
  (wrong "In external RAM"
    "External RAM holds nothing useful at power-on. The boot ROM, built into the bitstream, is there from the start, and it loads the rest.")
  (wrong "On the SD card"
    "The processor needs a program before it can read a card at all. That first program is the boot ROM, in block RAM.")
  (hint "Which memory is filled by the bitstream itself?"))
```

---

## A device with one register

The smallest useful device: one register the processor can write and
read back, whose bits drive eight LEDs (small lights).

```verilog
module led_reg (
    input  wire        clk,
    input  wire        reset,
    input  wire        sel,        // the bus selects this device
    input  wire        we,         // and it is a write
    input  wire [31:0] wdata,      // the data the CPU is writing
    output wire [31:0] rdata,      // the data the CPU reads
    output wire [7:0]  leds
);
    reg [7:0] value;

    always @(posedge clk)
        if (reset)
            value <= 8'd0;
        else if (sel && we)
            value <= wdata[7:0];

    assign rdata = {24'd0, value};
    assign leds  = value;
endmodule
```

`we` is **write enable**: the transfer is a write. The register loads
the bottom byte of the bus data at an edge when the device is selected
and the bus is writing.

```sources
(source "Timeless Computing (docs/tc.md), Systems on a Chip: Adding a Device, End to End")
```

```check
(number
  (ask "value holds 8'h05. The CPU writes 32'h0000_00FF with sel and we both 1. What does value hold after the edge, in decimal?")
  (answer 255) (from "15 * 16 + 15")
  (wrong-if 5 "With sel and we both 1, the register loads the bus data. It does not keep 5.")
  (wrong-if 260 "The register is loaded, not added to: it takes wdata[7:0], which is 8'hFF.")
  (hint "value <= wdata[7:0]: the bottom byte of what is written."))
```

---

## Selecting it

The address decoder for a system of a few devices can look at the top
four bits of the address alone:

```verilog
    // the address decoder: the top four bits pick the device
    wire sel = (addr[31:28] == 4'hE);
```

So the device answers at every address from `0xE000_0000` to
`0xEFFF_FFFF`. A fuller decoder checks more bits, so that several
devices can share that range.

```check
(choice
  (ask "With this decoder, does led_reg answer at address 0xE000_0004?")
  (right "Yes: its top four bits are E")
  (wrong "No: only 0xE000_0000 exactly"
    "The decoder looks at the top four bits only. Every address from 0xE000_0000 to 0xEFFF_FFFF has E there.")
  (wrong "No: the last digit must be 0"
    "The decoder never looks at the low bits. Only the first hex digit, E, matters.")
  (hint "addr[31:28] is the first hex digit of the address."))
```

---

## From program to light

The leds output goes to eight pins of the FPGA; the board's constraints
file says which. On the software side, a C program names the 32-bit
value at `0xE000_0000` `PINS`, and turns the first light on with
`PINS |= 1u`: read it, set bit 0, write it back.

1. The compiler turns that line into a load from `0xE000_0000`, an OR
   with 1, and a store back.
2. The load puts the address on the bus. The decoder sees `E` and
   selects `led_reg`, which puts its register on `rdata`.
3. The ALU, the CPU's arithmetic part, sets bit 0.
4. The store puts the address and the new value on the bus with `we`
   set. At the next clock edge, `value` loads it.
5. The `leds` output changes, the pin goes to 3.3 V, current flows
   through the LED, and the light comes on.

```check
(choice
  (ask "In step 4, what makes value load the new byte?")
  (right "sel and we are both 1 at a rising clock edge")
  (wrong "The CPU writes directly into the flip-flops"
    "The CPU only puts the address, the data and we on the bus. The device's own always block loads value when it is selected for a write.")
  (wrong "The load in step 2"
    "The load only reads value. It is the store, with we set, that makes value load.")
  (hint "Look at the if in led_reg's always block."))
```

---

```check
(choice
  (ask "A program writes to address 0xF000_0000. With the decoder above, does led_reg change?")
  (right "No: the top four bits are F, not E")
  (wrong "Yes: every write goes to every device"
    "Each device acts only when its select signal is 1, and the decoder raises led_reg's only for addresses whose first hex digit is E.")
  (wrong "Yes: the low bits are all 0, as for 0xE000_0000"
    "The decoder ignores the low bits and looks only at the top four. F is not E.")
  (hint "What is addr[31:28] for 0xF000_0000?"))
```

So: an SOC is bus masters, an arbiter, an address decoder and devices,
and one store instruction travels through all of them to light an LED.
