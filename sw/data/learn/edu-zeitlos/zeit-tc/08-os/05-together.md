# Several things at once

```lesson
(objective "Spot a race condition, protect a critical section, and avoid a deadlock.")
(minutes 20)
(keywords "thread" "race condition" "critical section" "atomic instruction" "lock (on data)" "deadlock")
```

Whenever two activities share data, the order of their steps matters.
The two can be processes using shared memory, two **threads** of one
program (parts of it that run on their own), or a program and an
interrupt handler.

Adding one to a counter in memory takes three instructions: load the
value, add one, store it. Suppose an interrupt comes between the
program's load and its store, and the handler also adds one:

| program | interrupt handler | counter |
|---|---|---|
| load counter (5) | | 5 |
| | load counter (5) | 5 |
| | add one, store (6) | 6 |
| add one, store (6) | | 6 |

```sources
(source "Timeless Computing (docs/tc.md), Operating Systems: Doing Several Things at Once")
```

```check
(number
  (ask "The counter started at 5, and both the program and the handler added one. What should it hold?")
  (answer 7) (from "5 + 1 + 1")
  (wrong-if 6 "6 is what it does hold. Two additions to 5 should give 7: one of them has been lost.")
  (hint "Two additions of one, starting from 5."))
```

---

## A race condition

The program stored its own result, 6, over the handler's, so the
handler's addition was lost. This is a **race condition**: the result
depends on the timing. It may happen once in a million runs, which
makes it one of the hardest mistakes to find.

The part of a program that must not be interrupted while it uses shared
data is a **critical section**.

```check
(choice
  (ask "A program with a race condition passes a thousand tests. What can you conclude?")
  (right "Little: the bad timing may simply not have happened yet")
  (wrong "That it has no race condition"
    "A race condition shows only when the timing goes wrong, perhaps once in a million runs. A thousand good runs do not rule it out.")
  (wrong "That the race condition has fixed itself"
    "The code is the same; only the timing differs from run to run. The mistake is still there.")
  (hint "When does a race condition show itself?"))
```

---

## Protecting a critical section

There are several ways:

- **Disable interrupts** for the few instructions of the critical
  section, on a single processor. Simple, but interrupts must not stay
  off for long.
- An **atomic instruction** reads, changes and writes a memory location
  in one step that cannot be divided. RISC-V has them in its A (atomic)
  extension.
- A **lock** is a value in memory that a process sets before it enters a critical
  section and clears afterwards; any other process that finds it set
  waits. The lock itself is set with an atomic instruction, or with
  interrupts off.
- **Messages**: if only one process owns the data, and others ask it
  for changes by message, nothing is shared.

```check
(choice
  (ask "Why must a lock itself be set with an atomic instruction, or with interrupts off?")
  (right "Otherwise two processes could both find it clear and both set it")
  (wrong "Because a lock is too large for ordinary instructions"
    "A lock can be a single word. The danger is a race on the lock itself: two processes each test it, see it clear, and both go in.")
  (wrong "So that it is faster"
    "It is about correctness, not speed. Testing and setting must be one step that nothing can come between.")
  (hint "Setting a lock is itself a load, a change and a store."))
```

---

## Deadlock

Locks bring their own danger. Process A holds lock 1 and waits for lock
2, while process B holds lock 2 and waits for lock 1. Both wait for
ever: a **deadlock**. It is avoided by always taking locks in the same
order.

```check
(choice
  (ask "Every process takes lock 1 before lock 2. Can the deadlock above happen?")
  (right "No: whoever has lock 1 can always go on to get lock 2")
  (wrong "Yes: two locks can always deadlock"
    "The deadlock needs one process to hold lock 2 while waiting for lock 1. If everyone takes lock 1 first, nobody holds lock 2 while waiting for lock 1.")
  (hint "For the deadlock, someone must hold lock 2 and want lock 1. Can that happen here?"))
```

---

```check
(choice
  (ask "A program and an interrupt handler both add to one counter. Which is the simplest fix on a single processor?")
  (right "Turn interrupts off around the program's load, add and store")
  (wrong "Make the handler run less often"
    "That makes the race rarer, not gone. Rarer is worse: it becomes harder to find.")
  (wrong "Add a second counter for the handler, and never read either"
    "A counter nobody reads is useless. The fix is to keep the handler out of the program's three instructions.")
  (hint "The handler must not run between the load and the store."))
```

So: when activities share data, protect each critical section, and take
locks in one order.
