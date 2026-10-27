# Principles

```lesson
(objective "Name the principles of a computer meant to last, and judge which ones a machine meets.")
(minutes 20)
(keywords "longevity (of a computer)" "recoverability" "durability (of a computer)" "repairability" "modularity" "openness (of a design)" "operating system (of a computer)" "documentation (of a machine)")
```

You will learn the principles of a computer meant to last, and use them
to judge real machines.

Few machines have all of them, but each one makes a machine more likely
to stay useful.

- **Purpose**: it is designed for timeless applications.
- **Longevity**: it stays useful for a century or more. Ideally this
  works both ways: a person from a century ago, given electricity and
  the instructions, could learn to use it too.
- **Recoverability**: it can always be put back into a known working
  state.
- **Durability**: it stands up to reasonable wear and handling.

```sources
(source "Timeless Computing (docs/tc.md), chapter 1, Principles")
```

```check
(choice
  (ask "After a mistake, your computer will not start properly, but you can always put it back as it was when it worked. Which principle is that?")
  (right "Recoverability")
  (wrong "Durability"
    "Durability is about standing up to wear and handling, such as being carried and dropped. Getting back to a known working state is recoverability.")
  (wrong "Longevity"
    "Longevity is staying useful for a century or more. Getting back to a known working state after a mistake is recoverability.")
  (hint "Which principle is about getting back, or recovering?"))
```

---

## Parts and repair

- **Replaceable parts**: parts that wear out, such as moving parts and
  some kinds of storage, can be replaced by the owner.
- **Repairability**: it can be put together and repaired with ordinary
  tools.
- **Long-term storage**: it can keep information for decades or
  centuries, on storage chosen for that.

```check
(choice
  (ask "A computer's case is glued shut, so that only the factory can open it. Which principle does that work against most?")
  (right "Repairability")
  (wrong "Purpose"
    "Purpose is what the computer is designed for. A case nobody can open with ordinary tools makes it hard to repair: repairability.")
  (wrong "Long-term storage"
    "Long-term storage is about keeping information for decades. A case that ordinary tools cannot open is about repair.")
  (hint "What can the owner not do with ordinary tools?"))
```

---

## Simple and open

- **Simplicity**: it is no more complex than its purpose needs, and it
  includes its own **documentation**: the written description of how
  it works and how to use it.
- **Preservation**: its designs, software and documentation are kept
  in public archives, in more than one place.
- **Modularity**: it can be extended by adding parts, called modules,
  instead of by replacing the whole machine.
- **Openness**: its designs for hardware and software are open for
  anyone to look at, repair and change.

```check
(choice
  (ask "You need your computer to read a new kind of card. You plug a small extra board into a socket on it, instead of buying a new computer. Which principle made that possible?")
  (right "Modularity")
  (wrong "Simplicity"
    "Simplicity is being no more complex than the purpose needs. Extending a machine by adding a part is modularity.")
  (wrong "Preservation"
    "Preservation is keeping the designs in public archives. Adding a part instead of replacing the machine is modularity.")
  (hint "The extra board is a module."))
```

---

## Openness helps the rest

Openness supports the other principles. A design that anyone can read
can be checked, repaired and built again by others, even after the
people who first made it are gone.

```check
(choice
  (ask "The company that made a computer closes. Why does it help if the computer's designs were open?")
  (right "Others can still read the designs, and repair or rebuild the machine")
  (wrong "It makes no difference once the company is gone"
    "With closed designs, nobody else knows how the machine works. With open ones, anybody can read them and keep repairing or rebuilding it.")
  (wrong "The company can still send updates"
    "The company is gone, so it sends nothing. The help comes from the designs themselves, which anyone can still read.")
  (hint "Who can read an open design?"))
```

---

## Many possible machines

Timeless computing is a way of choosing and using computers, not one
product. Many machines meet some of its principles:

- an older computer kept working, with its manuals and spare parts;
- an ordinary PC with a small, well-understood **operating system**
  (the software that runs the computer itself and starts its other
  programs), with plain text files;
- a small board that does one job for twenty years.

Each meets some principles better than others. An older computer is
simple, but its parts are no longer made. A PC is easy to get, but its
hardware designs are closed and its parts are not designed to last.

```sources
(source "Timeless Computing (docs/tc.md), chapter 1, Many Possible Machines")
```

```check
(choice
  (ask "An old computer from the 1980s is simple and still works, but when a part breaks, nobody makes a new one. Which principle does it fall short on?")
  (right "Replaceable parts")
  (wrong "Simplicity"
    "Simplicity is its strength: the lesson's example says an older computer is simple. Its weakness is that its parts are no longer made.")
  (wrong "Purpose"
    "It may well be used for timeless applications. The trouble is getting parts when one breaks.")
  (hint "What happens when a part breaks?"))
```

---

```check
(choice
  (ask "Which principle does an ordinary PC usually meet well?")
  (right "It is easy to get, and runs a small operating system if you choose one")
  (wrong "Openness: all its hardware designs are public"
    "A PC's hardware designs are usually closed: you cannot read how its parts are designed.")
  (wrong "Replaceable parts that last for decades"
    "Its parts are not designed to last, and are replaced by newer ones every few years.")
  (hint "The lesson named one strength and two weaknesses of a PC."))
```

So a machine meant to last is purposeful, recoverable, repairable,
simple, modular and open, and many kinds of machine can meet some of
those principles.
