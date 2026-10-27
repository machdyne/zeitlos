# The scheduler

```lesson
(objective "Say which process the Zeitlos scheduler runs next, on a tick and when a process blocks.")
(minutes 20)
(keywords "runnable" "hand-off")
```

You will learn how Zeitlos decides who uses the CPU.

A process is **runnable** when it is active and not blocked. The
scheduler shares the CPU among the runnable ones by round robin: on
each tick, the running process is interrupted and the next runnable
one in turn gets the CPU. A process that never blocks therefore runs
for one tick, about 1.37 ms, then waits for the others.

A process that is blocked takes no time at all. When nothing is
runnable, the CPU sleeps until the next interrupt.

```sources
(source "Zeitlos docs: kernel.md, The scheduler; CPU accounting (waitirq, idle)")
```

```check
(choice
  (ask "Three processes are runnable and none ever blocks. How does the CPU go round?")
  (right "Each runs for a tick in turn")
  (wrong "The first runs until it finishes, then the next"
    "That would be cooperative. Zeitlos is preemptive: the tick takes the CPU away from each in turn.")
  (wrong "The one with the lowest pid always runs"
    "There are no fixed priorities in the round robin. Each runnable process gets its turn."))
```

---

## When a process blocks

Most switches do not wait for a tick. A process that blocks, waiting
for a message, gives up the rest of its turn at once, and the scheduler
chooses the next one in this order:

1. the **hand-off**: if the process had just sent a message, the CPU
   goes straight to the process it sent it to, if that one can run;
2. otherwise a runnable driver;
3. otherwise round robin.

A request to a driver is "send, then wait for the answer". With the
hand-off, both trips go straight across. Measured with one busy process
running beside it, a request to the filesystem took about 2 ms with the
hand-off and about 8 ms without.

```sources
(source "Zeitlos docs: kernel.md, The scheduler (who goes next when a process blocks)")
```

```check
(choice
  (ask "The text editor sends a request to the filesystem driver and blocks to wait. The filesystem driver can run. Who gets the CPU?")
  (right "The filesystem driver, by the hand-off")
  (wrong "The next process in round robin"
    "Round robin comes last. A process that just sent a message hands the CPU to the receiver.")
  (wrong "Nobody until the next tick"
    "A process that blocks gives up its turn at once; the CPU does not sit idle while someone can run."))
```

---

## Wait by blocking

A program waiting for a message must block in the call that receives
it. A program that instead loops, asking again and again whether a
message has come, is runnable all the time: each turn it gets is a
whole tick of nothing, taken from everyone else.

The same holds for timers: a timeout of N ticks lasts between N and
N + 1 ticks, and the process sleeps blocked meanwhile.

```sources
(source "Zeitlos docs: kernel.md, The scheduler (waiting means blocking; timeouts)")
```

```check
(choice
  (ask "A program checks for a message in a tight loop instead of blocking. Two other programs are busy. What does the loop cost them?")
  (right "A third of the CPU: the loop takes its full tick every turn")
  (wrong "Nothing, because it does no real work"
    "Doing nothing while runnable still uses a tick each turn. Only a blocked process costs nothing.")
  (wrong "All of the CPU: the others never run"
    "Preemption still gives the others their turns. The loop only wastes its own share."))
```

---

## Practice

```check
(number
  (ask "A process asks to sleep for 10 ticks. At most how many ticks can pass before it runs again, if the CPU is free?")
  (answer 11) (from "10 + 1")
  (wrong-if 10 "A timeout ends one tick after its deadline tick, so it lasts between N and N + 1 ticks.")
  (hint "Between N and N + 1."))
```

```check
(choice
  (ask "Why does the desktop feel quick on a 48 MHz CPU?")
  (right "Waiting programs use no time, and work passes straight to the program that must answer")
  (wrong "Because the tick is very fast"
    "The tick is only about 732 a second. Responsiveness comes from blocking and the hand-off, not the tick.")
  (wrong "Because only one program runs at a time"
    "Many programs are open. Most are blocked and cost nothing, and requests go straight to their receivers."))
```

So: Zeitlos runs runnable processes in turn on each tick, but a process that blocks hands the CPU straight to the one it just sent a message to.
