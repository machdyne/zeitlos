# Fetch, decode, execute

```lesson
(objective "Follow a CPU through a short program, one fetch, decode and execute at a time.")
(minutes 20)
(keywords "fetch (an instruction)" "decode (an instruction)" "execute (an instruction)" "branch instruction" "loop (everyday word)")
```

This lesson follows a CPU as it runs a program, one instruction at a
time.

A CPU runs a program by doing three things, over and over:

1. **Fetch**: read the instruction at the address in the program
   counter.
2. **Decode**: work out what the instruction asks for: which operation,
   which registers, which constant.
3. **Execute**: do it, and write the result to a register or to memory.
   Then move the program counter on to the next instruction, 4 bytes
   further on, unless the instruction sends it somewhere else.

Every RISC-V instruction is 32 bits: 4 bytes. That is why the program
counter moves on by 4.

```sources
(source "Timeless Computing (docs/tc.md), The Computer: Fetch, Decode, Execute")
```

```check
(number
  (ask "The program counter holds 8, and the instruction there is an add. What does the program counter hold after it is executed?")
  (answer 12) (from "8 + 4")
  (wrong-if 9 "Memory is counted in bytes, and every instruction is 4 bytes long, so the next one starts 4 bytes on.")
  (hint "Each instruction takes 4 bytes."))
```

---

## A program

This RISC-V program adds the numbers from 10 down to 1. The words after
each `#` are comments, for people; the CPU never sees them.

```asm
        addi x5, x0, 10      # x5 = 0 + 10, the counter
        addi x6, x0, 0       # x6 = 0 + 0, the running total
loop:   add  x6, x6, x5      # x6 = x6 + x5
        addi x5, x5, -1      # x5 = x5 - 1
        bne  x5, x0, loop    # if x5 is not 0, go back to loop
```

The last line is a **branch instruction**: one that goes to another
place in the program if a condition holds. `bne` means "branch if not
equal". `loop` names the place it goes back to. A part of a program
that runs again and again is a **loop**.

```check
(choice
  (ask "x5 has just become 0, and the CPU runs bne x5, x0, loop. What happens next?")
  (right "The branch is not taken: the CPU goes on to the next instruction")
  (wrong "It goes back to loop"
    "bne branches only if the two registers are not equal. x5 is 0 and x0 is always 0: they are equal, so it does not branch.")
  (wrong "The CPU stops"
    "A CPU never simply stops at a branch. If the branch is not taken, the program counter moves on 4 bytes, as after any other instruction.")
  (hint "bne means branch if not equal. Are x5 and x0 equal now?"))
```

---

## Step through it

Press **Right** to take one step: a fetch, a decode or an execute. The
box shows the instruction being worked on, the arrow the address in
the program counter, and a thicker box a register that has just
changed. A ? means nothing has been written to that register yet.

```figure
(define k (slider 'step 0 96 0 "Step" 1))
(define i (min 32 (floor (/ k 3))))
(define ph (if (= k 96) 3 (modulo k 3)))
(define s (cp-run i))
(define t (if (= ph 2) (cp-step s) s))
(define row (/ (car s) 4))
(for-range 0 5
  (lambda (j)
    (let ((y (+ 34 (* j 20))))
      (text 20 y (string-append "0x" (cp-hex (* 4 j) 2)) 'small)
      (if (= j 2) (text 54 y "loop:" 'small))
      (text 94 y (car (list-ref cp-prog j)) 'small))))
(text 20 134 "0x14" 'small 'grey)
(if (< ph 3) (frame 16 (+ 20 (* row 20)) 202 19 'ink 1.5))
(define prow (/ (car t) 4))
(arrow 2 (+ 30 (* prow 20)) 14 (+ 30 (* prow 20)) 'ink 2)
(define (reg y name v w)
  (frame 230 y 84 26 'ink w)
  (text 238 (+ y 18) name 'body 'bold)
  (text 306 (+ y 18) v 'body 'right))
(define (moved r) (and (= ph 2) (not (equal? (r s) (r t)))))
(reg 20 "PC" (string-append "0x" (cp-hex (car t) 2)) (if (moved car) 3 1))
(reg 54 "x5" (cp-val (cadr t)) (if (moved cadr) 3 1))
(reg 88 "x6" (cp-val (caddr t)) (if (moved caddr) 3 1))
(define in (if (< i 32) (cp-ins s) '(addi 0 0 0)))
(define op (car in))
(define a (cadr in))
(define b (caddr in))
(define c (cadddr in))
(define (xr r) (string-append "x" (fmt r 0)))
(define (plus c) (if (< c 0) (string-append " - " (fmt (- c) 0)) (string-append " + " (fmt c 0))))
(define go (if (eq? op 'bne) (not (= (cp-get s a) (cp-get s b))) #f))
(text 16 160 (list-ref '("1  Fetch" "2  Decode" "3  Execute" "Done") ph) 'body 'bold)
(define l1
  (cond ((= ph 0) (string-append "Read the word at address 0x" (cp-hex (car s) 2) ","))
        ((= ph 3) "PC = 0x14, past the last instruction.")
        ((= ph 1)
         (cond ((eq? op 'bne) (string-append "bne: go to loop if " (xr a) " is not " (xr b) "."))
               ((eq? op 'add) (string-append "add: " (xr a) " = " (xr b) " + " (xr c) "."))
               (else (string-append "addi: " (xr a) " = " (xr b) (plus c) "."))))
        ((eq? op 'bne)
         (string-append "x5 is " (fmt (cp-get s a) 0) (if go ", not 0: branch taken." ", which is 0: not taken.")))
        ((eq? op 'add)
         (string-append (xr a) " = " (fmt (cp-get s b) 0) " + " (fmt (cp-get s c) 0) " = " (fmt (cp-get t a) 0)))
        (else (string-append (xr a) " = " (fmt (cp-get s b) 0) (plus c) " = " (fmt (cp-get t a) 0)))))
(define l2
  (cond ((= ph 0) "the address in the PC.")
        ((= ph 1) "Which operation, which registers, which constant.")
        ((= ph 3) "x6 holds the total.")
        (go "PC = 0x08: back to loop.")
        (else (string-append "PC = 0x" (cp-hex (car t) 2) ": 4 bytes on."))))
(text 16 182 l1 'small)
(text 16 200 l2 'small)
(caption (string-append "Instructions run: " (fmt (if (= ph 2) (+ i 1) i) 0)))
```

```check
(number
  (ask "Step through the first pass of the loop. After the first add, what is in x6?")
  (answer 10) (from "0 + 10")
  (wrong-if 0 "Look at the step after the add's execute: x6 = x6 + x5, and x5 is 10.")
  (hint "Before the add, x6 is 0 and x5 is 10."))
```

---

```check
(number
  (ask "Go on to the end. How many instructions does the CPU run in all?")
  (answer 32) (from "2 + 3 * 10")
  (wrong-if 5 "The program has 5 lines, but the loop's 3 instructions run once for each pass.")
  (wrong-if 30 "That is the 10 passes of the loop. The 2 instructions before it run too.")
  (hint "2 instructions before the loop, then 3 for each of 10 passes."))
```

---

```check
(number
  (ask "What is in x6 when the program ends?")
  (answer 55) (from "10 + 9 + 8 + 7 + 6 + 5 + 4 + 3 + 2 + 1")
  (wrong-if 45 "That leaves out the 10. The first add adds x5 while it is still 10.")
  (hint "The program adds 10 + 9 + ... + 1."))
```

---

## A change to the program

```example
(example
  (problem "The first line becomes addi x5, x0, 3. What does x6 hold at the end?")
  (step "x5 starts at 3, so the loop runs while x5 counts 3, 2, 1.")
  (step "Each pass adds x5 to x6: 3, then 2, then 1.")
  (answer "x6 = 3 + 2 + 1 = 6."))
```

```check
(number
  (ask "With that change, how many instructions does the CPU run in all?")
  (answer 11) (from "2 + 3 * 3")
  (wrong-if 9 "That is 3 passes of 3 instructions. The 2 before the loop count too.")
  (wrong-if 32 "That was with 10 passes. Now x5 starts at 3, so the loop runs 3 times.")
  (hint "2 instructions, then 3 for each pass. How many passes now?"))
```

---

```check
(choice
  (ask "Why does the CPU fetch the add at 0x08 ten times, though it is written once?")
  (right "The branch sets the program counter back to 0x08")
  (wrong "The CPU repeats every instruction ten times"
    "Each fetch runs one instruction once. The instructions before the loop run only once; the loop repeats because the branch sends the program counter back.")
  (wrong "The program is stored ten times in memory"
    "It is stored once. A taken branch puts loop's address, 0x08, back into the program counter.")
  (hint "What does a taken branch do to the program counter?"))
```

So: a CPU fetches the instruction at the program counter, decodes it,
executes it, and moves on, unless a branch sends it elsewhere.
