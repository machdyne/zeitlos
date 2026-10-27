# Memory, addresses and the stack

```lesson
(objective "Find a byte in memory by its address, move words with loads and stores, and say how the stack lets subroutines call each other.")
(minutes 25)
(keywords "load-store" "little-endian" "big-endian" "subroutine" "return address" "stack (in memory)" "stack pointer" "stack frame" "stack overflow")
```

This lesson is about how a program finds things in memory.

Memory is a long row of bytes. Each byte has a number of its own, its
**address**, counting up from 0. A 32-bit processor can name 2 to the
32nd addresses: 4,294,967,296 bytes, 4 GB in the binary sense. A small
computer has far less memory than that, and the other addresses are
used for other things, as the next lesson shows.

```sources
(source "Timeless Computing (docs/tc.md), The Computer: Memory and Addresses")
```

```check
(number
  (ask "A small processor has 16-bit addresses. How many bytes can it name?")
  (answer 65536) (from "2^16")
  (wrong-if 16 "16 is the number of address bits. Each bit doubles the number of addresses: 2 to the 16th.")
  (wrong-if 65535 "65,535 is the highest address. Counting address 0, there are 65,536.")
  (hint "n bits make 2 to the n patterns, and each pattern is one address."))
```

---

## Loads and stores

RISC-V is a **load-store** design: arithmetic works only on registers,
and separate instructions move data between registers and memory. A
**load** copies from memory into a register; a **store** copies from a
register into memory.

```asm
        lw   x7, 0(x10)      # load the word at address x10 + 0 into x7
        sw   x7, 4(x10)      # store x7 at address x10 + 4
```

The address is a register plus a constant. lw and sw move a whole
32-bit word, four bytes.

```check
(number
  (ask "x10 holds 100. At what address does sw x7, 4(x10) store?")
  (answer 104) (from "100 + 4")
  (wrong-if 4 "The 4 is added to the register. The address is x10 + 4.")
  (wrong-if 100 "That is where x10 points. The 4 in front of (x10) is added to it.")
  (hint "The address is the register's value plus the number in front."))
```

---

## Which byte first

A 32-bit word takes four bytes in a row. RISC-V puts the least
significant byte at the lowest address. This is called
**little-endian**. Some other processors put the most significant byte
first, **big-endian**; data moved between the two must have its bytes
swapped.

```figure
(define big (choice 'order '("little-endian" "big-endian")))
(text 160 36 "The word 0x12345678, stored at address 100" 'centre 'small)
(define bytes (if (= big 1) '("12" "34" "56" "78") '("78" "56" "34" "12")))
(for-range 0 4
  (lambda (k)
    (let ((x (+ 32 (* k 66))))
      (text (+ x 28) 82 (fmt (+ 100 k) 0) 'centre 'small)
      (frame x 90 56 40 'ink 1.5)
      (text (+ x 28) 116 (string-append "0x" (list-ref bytes k)) 'centre 'body 'bold))))
(text 32 150 "lowest address" 'tiny)
(text 286 150 "highest" 'tiny 'right)
(caption (if (= big 1) "Big-endian: the most significant byte first"
             "Little-endian, as in RISC-V: the least significant byte first"))
```

```check
(text
  (ask "On RISC-V, 0x12345678 is stored at address 100. Which byte is at address 101? Write it in hex.")
  (accept "56") (accept "0x56")
  (wrong-if "34" "That is where big-endian puts it. RISC-V is little-endian: 0x78 at 100, then 0x56.")
  (wrong-if "78" "0x78, the least significant byte, is at 100, the lowest address. 101 holds the next one.")
  (hint "Little-endian: start from the right of 0x12345678, one byte at address 100, the next at 101."))
```

---

## Subroutines

Programs reuse a piece of code by calling it as a **subroutine**, also
called a procedure. The instruction `jal` (jump and link)
jumps to the subroutine and saves the address of the instruction after
it, the **return address**, by custom in register x1. At its end, the
subroutine jumps back to that address with `jalr`.

```check
(text
  (ask "A jal instruction is at address 0x40. What return address does it save in x1? Write it in hex.")
  (accept "44") (accept "0x44")
  (wrong-if "40" "Returning to 0x40 would run the jal again, and call the subroutine forever. The return address is the next instruction.")
  (wrong-if "41" "Instructions are 4 bytes long, so the next one is 4 bytes on.")
  (hint "Where does the next instruction after the jal start?"))
```

---

## The stack

A subroutine that calls another would overwrite x1, so first it saves
the old value. It saves it on the **stack**: an area of memory where the
last thing put on is the first taken off. Register x2, the
**stack pointer** (sp), holds the address of the top of the stack.

A subroutine makes room by lowering sp, and saves there the registers
it must keep. Before it returns, it puts them back and raises sp again.
Each call has its own **stack frame**, which also holds its own
working values. On RISC-V the stack grows down, towards lower addresses.

```figure
(define d (slider 'calls 0 3 1 "Calls" 1))
(define names '("main" "draw" "line" "dot"))
(text 100 22 "higher addresses" 'tiny)
(for-range 0 (+ d 1)
  (lambda (k)
    (let ((y (+ 30 (* k 36))))
      (frame 100 y 120 36 'ink 1.5)
      (text 160 (+ y 16) (string-append (list-ref names k) "'s frame") 'centre 'small 'bold)
      (text 160 (+ y 29) "return address and more" 'centre 'tiny))))
(for-range 0 (+ d 2)
  (lambda (k)
    (text 94 (+ 34 (* k 36)) (string-append "0x" (cp-hex (- 4096 (* 16 k)) 4)) 'right 'tiny)))
(define y (+ 30 (* 36 (+ d 1))))
(frame 100 y 120 (- 200 y) 'grey 1)
(text 160 (+ y 18) "free" 'centre 'small 'grey)
(arrow 290 y 224 y 'ink 2)
(text 300 (- y 6) "sp" 'right 'body 'bold)
(text 312 (+ y 14) (string-append "0x" (cp-hex (- 4096 (* 16 (+ d 1))) 4)) 'right 'small)
(text 100 216 "lower addresses" 'tiny)
(caption "Each call here lowers sp by 16 bytes")
```

```check
(text
  (ask "sp holds 0x1000. A subroutine makes a stack frame of 16 bytes. What does sp hold now? Write it in hex.")
  (accept "FF0") (accept "0xFF0") (accept "0FF0") (accept "0x0FF0")
  (wrong-if "1010" "The stack grows down, towards lower addresses. Making room lowers sp.")
  (wrong-if "0x1010" "The stack grows down, towards lower addresses. Making room lowers sp.")
  (wrong-if "FF0F" "16 is 0x10 in hex: take 0x10 from 0x1000.")
  (hint "16 is 0x10. Take it away from 0x1000."))
```

---

## Overflow

The stack lets one subroutine call another, which calls another, as
deep as memory allows, and gives each call its own copies of its
working values. A program that runs out of room for its stack, usually
because a subroutine calls itself without ever stopping, has a **stack
overflow**.

```check
(choice
  (ask "A subroutine calls itself, and has no test that ever stops it. What happens?")
  (right "Each call adds a stack frame until the stack runs out of room")
  (wrong "It runs forever without harm"
    "A plain loop could run forever. Each call, though, lowers the stack pointer by another stack frame, and memory runs out.")
  (wrong "The CPU notices and stops the second call"
    "The CPU does not know what the program means. It runs each jal, and each call takes another stack frame.")
  (hint "What does every call do to the stack pointer?"))
```

So: every byte of memory has an address, loads and stores move words
between memory and registers, and the stack keeps each call's return
address and working values.
