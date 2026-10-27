# The MPU

```lesson
(objective "Say what the Zeitlos MPU lets a program do, and what happens when a program breaks its rules.")
(minutes 20)
(keywords "privileged code" "gate (into the kernel)" "crash report")
```

You will learn how Zeitlos keeps one faulty program from harming the
others.

Every Zeitlos board builds the memory protection unit, `rtl/mpu.v`. It
sits on the CPU's path beside the MTU and checks every fetch, load and
store before it reaches the cache or the bus. It adds no cycles.

Its purpose is stated plainly: Zeitlos is for one person, who chooses
the programs, so the MPU is there to **contain mistakes**, not to keep
secrets. A wild pointer ends that program with a report, instead of
quietly corrupting another program or the kernel.

```sources
(source "Zeitlos docs: mpu.md, the introduction; Summary; Where it sits")
```

```check
(choice
  (ask "What is the Zeitlos MPU for?")
  (right "Stopping a buggy program from damaging other programs and the kernel")
  (wrong "Keeping each program's data secret from the others"
    "Any program may read nearly anything: messages depend on it. The MPU stops harmful writes and jumps, not reading.")
  (wrong "Translating each program's addresses"
    "That is the MTU. The MPU checks accesses; it changes no address.")
  (hint "Containment, not secrecy."))
```

---

## Who is privileged

There is no mode bit that software sets. **Privileged code** is simply
code that was fetched from the boot RAM (`0x0000_0000` to `0x0000_1FFF`)
or from the kernel's own code at the start of main memory. It may do
anything. All other code, every app, follows the rules on the next
step.

An app may enter kernel code at only two places, the **gates**: the
system call entry, and the interrupt entry at `0x10`. A program that
jumps anywhere else in the kernel is stopped, so a broken pointer cannot
make it privileged.

```sources
(source "Zeitlos docs: mpu.md, The rules (privileged code; the gate)")
```

```check
(choice
  (ask "An app has a corrupted function pointer that points into the middle of the kernel. What happens when it calls it?")
  (right "The fetch is blocked, because kernel code may only be entered at a gate")
  (wrong "The app runs kernel code with the kernel's privileges"
    "That is exactly what the gates prevent. Only the system call entry and the interrupt entry are open to an app.")
  (wrong "Nothing special: the kernel's code is just memory"
    "Privilege comes from where code is fetched, so jumping into the kernel would be dangerous. The MPU allows it only at the gates."))
```

---

## The rules for an app

| An app may | Where |
|---|---|
| run instructions | only in its own block (and at the gates) |
| store | in its own block, and in the device regions it is allowed |
| store, as well | in one granted window: its arena, or a driver's client |
| load | anywhere it is allowed, which is nearly everywhere |

Some stores are the kernel's alone, whatever: the boot RAM, the flash,
main memory outside the app's block, the cache controls, the key that
makes the FPGA load its design again, and the MTU and MPU registers. A blocked store is dropped, a blocked load
reads 0, and a blocked fetch gives the CPU an illegal instruction.

A 16-bit mask says which regions an app may use at all. The kernel sets
it to `0xF7FF`: everything except region B, the SD card, which only drivers, the
filesystem driver above all, may use.

```sources
(source "Zeitlos docs: mpu.md, The rules (the table; kernel-only for stores; MASK)")
```

```check
(choice (several)
  (ask "Which of these may an ordinary app do? Choose all that apply.")
  (right "Read the kernel's memory")
  (right "Write to the blitter's registers")
  (right "Run code in its own block")
  (wrong "Write the MTU's base register"
    "The MTU and MPU registers, region 9, are written only by the kernel. Otherwise an app could map anyone's memory into its window.")
  (wrong "Write into another app's block"
    "Main memory outside an app's own block is kernel-only for stores.")
  (hint "Reading is nearly free; writing is limited to its own block and the devices."))
```

---

## The mask, bit by bit

Bit N of the mask stands for region N, with bit 0 for region 0.

```check
(number
  (ask "Which bit of 0xF7FF is 0?")
  (answer 11)
  (wrong-if 3 "Count from the right. 0xF7FF is 1111 0111 1111 1111; the 0 is in the third nibble from the right.")
  (wrong-if 7 "The digit 7 is the third nibble, bits 8 to 11. 7 is 0111, so its top bit, bit 11, is 0.")
  (hint "Write 0xF7FF in binary: 1111 0111 1111 1111.")
  (hint "The 7 is bits 8 to 11, and 0111 has its 0 at the top."))
```

---

## A crash report

When the MPU stops an app, the kernel ends that app and prints a
report on its console, such as:

```text
*** view (pid 7) crashed: store outside its own memory
    address 4001f2c0 (kernel memory)  at pc 800012a4
    ...
    ended; the rest of the system is unaffected
```

The CPU's own faults, an illegal instruction or a misaligned access,
are reported the same way. A fault in kernel code is different: nothing
can be trusted then, so the kernel prints a panic report, draws the end
of its log on the screen, and halts.

```sources
(source "Zeitlos docs: mpu.md, Crash reporting")
```

```check
(choice
  (ask "In the report above, where was the faulty instruction?")
  (right "In view's own code, in the app window")
  (wrong "In the kernel"
    "The pc, 800012a4, starts with 8: it is in the app window. The address the store aimed at, 4001f2c0, was kernel memory.")
  (wrong "In the boot RAM"
    "The boot RAM is region 0. The pc starts with 8.")
  (hint "Look at the first hex digit of the pc."))
```

So: the MPU lets an app run only its own code, write only its own memory and allowed devices, and enter the kernel only at its gates; anything else ends the app with a report.
