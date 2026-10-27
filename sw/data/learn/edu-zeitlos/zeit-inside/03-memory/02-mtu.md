# The MTU

```lesson
(objective "Translate an address in the app window to the physical address the MTU sends it to, and back.")
(minutes 20)
(keywords "translation base" "physical address" "virtual address")
```

You will learn how every Zeitlos program can live at the same address.

Every app is built to run at `0x8000_0000`, the start of the app window.
Each is loaded somewhere else in main memory. The MTU (`rtl/mtu.v`)
joins the two. It has a single register, the **translation base**, at
`0x9000_0000`. For every address the CPU sends out:

- if the first hex digit is 8 and the base is not 0, the MTU replaces
  the 8 by adding the base: **physical address** = base + the other 7
  digits;
- any other address passes through unchanged.

The address a program uses in the window is its **virtual address**;
the one that reaches the bus is the physical address.

```sources
(source "Zeitlos source: rtl/mtu.v (TRANSLATE_ADDR 0x8000_0000, TRANSLATE_MASK 0xF000_0000)")
(source "Zeitlos docs: kernel.md, Processes")
```

```check
(choice
  (ask "The base is 0x4010_0000. The program reads 0x2000_0010. Which address reaches the bus?")
  (right "0x2000_0010, unchanged")
  (wrong "0x6010_0010"
    "Only addresses whose first digit is 8 are translated. 0x2000_0010 is VRAM, and it passes straight through.")
  (wrong "0x4010_0010"
    "That would be the translation of 0x8000_0010. This address starts with 2, so the MTU leaves it alone.")
  (hint "Is the first hex digit 8?"))
```

---

## Watch it translate

Choose a program and move its address in the window. The MTU adds that
program's base.

```figure
(define who (choice 'prog '("program A" "program B") "Running"))
(define off (slider 'off 0 61440 4096 "Offset" 4096))
(define base (if (= who 0) 1074790400 1078898688))
(define virt (+ 2147483648 off))
(define phys (+ base off))
(frame 16 40 110 120 'ink 1)
(text 71 32 "app window" 'small 'centre)
(frame 196 40 110 160 'ink 1)
(text 251 32 "main memory" 'small 'centre)
(rect 197 (if (= who 0) 60 120) 108 50 'grey)
(frame 197 60 108 50 'ink 1)
(frame 197 120 108 50 'ink 1)
(text 312 90 "A" 'small 'centre)
(text 312 150 "B" 'small 'centre)
(define vy (+ 42 (* 110 (/ off 65536))))
(define py (+ (if (= who 0) 61 121) (* 48 (/ off 65536))))
(line 17 vy 125 vy 'ink 2)
(line 197 py 305 py 'ink 2)
(arrow 128 vy 194 py 'ink 1.5)
(text 16 182 (string-append "virtual  " (zi-addr virt)) 'small)
(text 16 200 (string-append "base     " (zi-addr base)) 'small)
(text 16 218 (string-append "physical " (zi-addr phys)) 'small 'bold)
```

```check
(text
  (ask "Program B's base is 0x404E_B000. It reads 0x8000_1234. What is the physical address? (Eight hex digits.)")
  (accept "0x404EC234") (accept "404EC234") (accept "0x404E_C234") (accept "404E_C234")
  (wrong-if "0x404EB000" "That is the base alone. Add the offset, 0x1234.")
  (wrong-if "0xC04EC234" "The 8 is not added: the MTU replaces it. Only the last 7 digits, 0x000_1234, are added to the base.")
  (hint "Keep the last 7 digits, 0x000_1234, and add them to the base.")
  (hint "0xB000 + 0x1234 = 0xC234."))
```

---

## One switch per context switch

When the kernel switches from one program to another, it writes the
next program's base into the MTU. From then on, `0x8000_0000` means that
program's memory. So:

- **every app is built to run at the same address**, and loading one is just
  a copy: nothing in it has to be adjusted for where it lands;
- every byte still has **one physical address**, which any program can
  read;
- only the CPU's addresses are translated. The blitter, a bus master
  of its own, must be given physical addresses.

Since the MPU came, only the kernel may write the MTU's base.

```sources
(source "Zeitlos docs: kernel.md, Processes (three consequences); gpu_blitter.md, Source addresses are physical; mpu.md, The MTU register is now kernel-only")
```

```check
(choice
  (ask "An app passes a pointer from its own memory, 0x8000_4000, straight to the blitter as a source address. What does the blitter read?")
  (right "Whatever is at physical 0x8000_4000, not the app's data")
  (wrong "The app's data, because the MTU translates it"
    "The MTU translates only the CPU's own addresses. The blitter's reads do not pass through it.")
  (wrong "The blitter refuses the address"
    "The blitter takes any address it is given. That is why the drawing library translates the pointer first.")
  (hint "Which master's addresses does the MTU see?"))
```

---

## And back again

The kernel often goes the other way: a program hands it a pointer, and
the kernel wants the physical address. That is the same sum:

physical = virtual - `0x8000_0000` + base

```check
(text
  (ask "Program A's base is 0x4010_0000. Which virtual address does A use for the physical address 0x4010_2000?")
  (accept "0x80002000") (accept "80002000") (accept "0x8000_2000") (accept "8000_2000")
  (wrong-if "0x00002000" "That is the offset alone. In the app's view it is inside the window, which starts at 0x8000_0000.")
  (wrong-if "0x4010_2000" "That is the physical address. The app sees its memory at 0x8000_0000 and up.")
  (hint "Subtract the base to get the offset, then add 0x8000_0000."))
```

---

## Practice

```check
(text
  (ask "The base is 0x4200_0000. What is the physical address of 0x8000_0010?")
  (accept "0x42000010") (accept "42000010") (accept "0x4200_0010") (accept "4200_0010")
  (wrong-if "0xC2000010" "The MTU replaces the 8 with the base. It does not add 0x8000_0000 as well.")
  (hint "Base plus 0x10."))
```

```check
(choice
  (ask "Why can every Zeitlos app be built to run at the same address?")
  (right "The MTU shows each one its own block at 0x8000_0000")
  (wrong "Only one app runs at a time, so they never meet"
    "Many apps are in memory at once, each in its own block. The MTU shows the running one at 0x8000_0000.")
  (wrong "The kernel moves each app to 0x8000_0000 before it runs"
    "Nothing is moved. The kernel writes one register, the base, and the addresses follow."))
```

So: the MTU adds the running program's base to every address in region 8, so all programs run at `0x8000_0000` while each byte keeps one physical address.
