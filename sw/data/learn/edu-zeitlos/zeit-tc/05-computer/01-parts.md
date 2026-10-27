# The parts of a computer

```lesson
(objective "Name the four parts every computer has, and say what each one does.")
(minutes 15)
(keywords "stored program" "central processing unit" "CPU" "input and output" "I/O" "bus (of a computer)")
```

This lesson names the four parts that every computer has, from a small
module to a data centre.

A computer follows **instructions**: small orders such as "add these two
numbers" or "jump back to there". A list of instructions is a
**program**. A computer keeps its program in its own **memory**, the
part that holds numbers for it, beside the data the program works on.
This idea, the **stored program**, is what makes a computer more than a
calculator.

```sources
(source "Timeless Computing (docs/tc.md), The Computer")
```

```check
(choice
  (ask "A pocket calculator does arithmetic, too. What makes a computer different?")
  (right "It follows a program kept in its own memory")
  (wrong "It can add and multiply"
    "A calculator adds and multiplies as well. The difference is where the steps come from: a calculator does one step each time you press a key; a computer reads its own list of steps from memory.")
  (wrong "It has a screen"
    "Many calculators have a screen, and some computers have none. What matters is the stored program.")
  (hint "Think about where the next step comes from: a person's finger, or the machine's memory?"))
```

---

## Four parts

```figure
(frame 20 24 90 44 'ink 2)
(text 65 52 "CPU" 'centre 'big 'bold)
(frame 210 24 90 44 'ink 2)
(text 255 52 "memory" 'centre 'body 'bold)
(rect 20 112 280 8 'ink)
(text 160 106 "bus" 'centre 'body 'bold)
(line 65 68 65 112 'ink 3)
(line 255 68 255 112 'ink 3)
(line 80 120 80 156 'ink 3)
(line 240 120 240 156 'ink 3)
(frame 20 156 120 44 'ink 1.5)
(text 80 174 "keyboard," 'centre)
(text 80 190 "storage ..." 'centre)
(frame 180 156 120 44 'ink 1.5)
(text 240 174 "screen," 'centre)
(text 240 190 "network ..." 'centre)
(caption "input and output devices (I/O)")
```

- The **central processing unit** (**CPU**), or processor, follows the
  instructions.
- **Memory** holds the instructions and the data they work on.
- **Input and output** devices (**I/O**) connect the computer to
  people and to other machines: a keyboard, a screen, a network.
- The **bus** is the set of wires, and the rules for using them, that
  joins the other parts.

```check
(choice
  (ask "Which part carries numbers between the CPU and memory?")
  (right "The bus")
  (wrong "The CPU"
    "The CPU works with the numbers once they arrive. The wires that bring them, and the rules for using those wires, are the bus.")
  (wrong "An I/O device"
    "I/O devices connect the computer to the world outside. Inside, the parts are joined by the bus.")
  (hint "Look at the figure: what is drawn between all the other parts?"))
```

---

```check
(choice
  (ask "Which of these are input and output devices?")
  (several)
  (right "A keyboard")
  (right "A screen")
  (right "A network connection")
  (wrong "The memory that holds the program in use"
    "Memory is one of the computer's own parts. I/O devices connect the computer to people and other machines.")
  (hint "Which ones let the computer take things in from outside, or give things out?"))
```

---

## Programs are numbers

Inside the computer, an instruction is a number, like any other data.
So a new program is loaded into memory the same way as any other data:
from storage, a network or a keyboard. A program can even write another
program, as a translator from one language to another does.

```check
(choice
  (ask "You want a computer to do a new job. What has to change?")
  (right "Only the numbers in its memory: a new program is loaded")
  (wrong "Its wires have to be connected differently"
    "Some of the very first machines were rewired for each job. A stored-program computer is not: the job is a program, and a program is numbers in memory.")
  (wrong "A new CPU has to be fitted"
    "The same CPU runs every program. It follows whatever instructions are in memory.")
  (hint "Where does a computer keep the list of steps it follows?"))
```

---

```check
(choice
  (ask "A computer is about to add two numbers that are in memory. Which part does the adding?")
  (right "The CPU")
  (wrong "The memory"
    "Memory only holds the numbers. The CPU reads them over the bus, adds them, and can write the result back.")
  (wrong "The bus"
    "The bus carries the numbers to the CPU, but it does no arithmetic.")
  (hint "Which part follows the instructions?"))
```

So: every computer has a CPU that follows a stored program, memory that
holds it, I/O devices, and a bus that joins them.
