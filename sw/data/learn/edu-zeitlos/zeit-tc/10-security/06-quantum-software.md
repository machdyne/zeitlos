# Quantum computers, deleting and safe software

```lesson
(objective "Say what quantum computers would break, how to really delete data, and the habits that make software safer.")
(minutes 20)
(keywords "quantum computer" "post-quantum" "ML-KEM" "ML-DSA" "hybrid (scheme)" "wear levelling" "buffer overflow" "least privilege")
```

A **quantum computer** computes with quantum states instead of ordinary
bits. One large enough would break the public-key methods in common use,
including RSA, X25519 and Ed25519. As of 2026, when this course was
written, no quantum computer that large has been built.

Symmetric encryption and hash functions are much less affected: a
256-bit key remains secure. So is the one-time pad, because its safety
does not depend on any calculation being hard.

```sources
(source "Timeless Computing (docs/tc.md), Security: Quantum Computers")
```

```check
(choice
  (ask "If a large quantum computer is built, which of these is no longer safe?")
  (right "A key exchange made with X25519")
  (wrong "A file encrypted with AES and a 256-bit key"
    "Symmetric encryption is much less affected. A 256-bit key remains secure; it is the public-key methods that fall.")
  (wrong "A message sent with a one-time pad"
    "The one-time pad rests on the randomness of its key, not on a hard calculation, so no computer of any kind can break it.")
  (hint "Which kind of method rests on a calculation that is hard to reverse?"))
```

---

## Recorded now, read later

Messages recorded today and kept could be decrypted later, once such
computers exist. So new **post-quantum** public-key methods, built on
problems quantum computers are not known to solve, have been made into
standards: **ML-KEM** for key exchange and **ML-DSA** for signatures,
published as United States federal standards in 2024.

They are newer and less studied. So careful systems use them in a
**hybrid** scheme, together with a classical method: the result is
secure as long as either one holds. Zeitlos's message protocol does
this, with X25519 and ML-KEM together ([fed](docs:fed)).

```sources
(source "Zeitlos docs: fed.md, Quantum computers; mlkem.md")
```

```check
(choice
  (ask "A hybrid key exchange uses X25519 and ML-KEM together. Then ML-KEM turns out to have a flaw. Is the connection still secure?")
  (right "Yes, as long as X25519 holds")
  (wrong "No: one broken half breaks the whole"
    "A hybrid is built so that an attacker must break both. With X25519 still sound, the shared key stays secret.")
  (hint "A hybrid is secure as long as how many of its two methods hold?"))
```

---

## Deleting is not erasing

Deleting a file usually removes only its name and marks its space as
free. The data stays on the card until something is written over it,
and can often be recovered.

On flash, even writing over it is not reliable. Flash wears out with
writing, so it spreads its writes about: with **wear levelling**, a new
version of a block goes to a different place, and the old one may
remain, hidden from the computer but readable by someone who reads the
chip directly.

The dependable way is to keep sensitive data encrypted from the start.
Then destroying the key destroys the data, wherever its copies lie.

```check
(choice
  (ask "You must give away a flash drive that held secrets. Which is dependable?")
  (right "It held only encrypted data, and you destroy the key")
  (wrong "Delete every file on it"
    "Deleting removes the names and marks the space free. The data stays until it is written over, and can often be recovered.")
  (wrong "Write over every file with zeros"
    "On flash, wear levelling sends the new writes to other places, so old copies may remain where the computer cannot see them.")
  (hint "Which way works wherever the old copies of the data are?"))
```

---

## Trust no input

Every byte that arrives from a network, a file or a device may have
been made to cause harm. A program that assumes its input has a certain
length can be made to misbehave. In a **buffer overflow**, a program
writes more bytes into a space in memory than it holds, over whatever
comes next: in the worst case, instructions the attacker supplied.

So input is checked before it is used, and programs that handle input
from outside are kept small and simple.

```check
(choice
  (ask "A program keeps names in a space of 32 bytes. A name of 300 bytes arrives from the network. What should the program do?")
  (right "Check the length first, and refuse or cut the name")
  (wrong "Copy it in: names are never that long"
    "Normal names are not, but input can be made by someone who wants to cause harm. Copying 300 bytes into 32 writes over whatever comes next.")
  (hint "Who chose that the name would be 300 bytes long?"))
```

---

## Limiting the damage

- **Least privilege.** Each program has only the access it needs. A
  program that cannot write to the system's files cannot damage them.
- **Few services.** Every program that accepts connections from a
  network is a possible way in.
- **Protection boundaries.** Memory protection keeps one program from
  damaging another by mistake. Whether a hostile program can cross it
  depends on its design. Zeitlos uses its memory protection to contain
  mistakes only: every app is trusted, and any app can read any address
  ([memory protection](docs:mpu)).
- **Updates.** Weaknesses are found in all software, and fixes come as
  updates. A small system has less code to hide them.

```sources
(source "Timeless Computing (docs/tc.md), Security: Secure Software")
(source "Zeitlos docs: mpu.md; security.md, What it protects, and what it does not")
```

```check
(choice
  (ask "On Zeitlos, is it safe to run an app from an unknown source, because memory protection keeps apps apart?")
  (right "No: memory protection there contains mistakes, but every app is trusted")
  (wrong "Yes: a hostile app cannot touch another app's memory"
    "Zeitlos's memory protection stops a faulty app from damaging others, but it is not built to stop a hostile one: any app can read any address.")
  (hint "What is Zeitlos's memory protection built to stop: mistakes, or attacks?"))
```

---

## Trusting the tools

Every program on a machine is trusted to do what it claims, including
the compiler: the program that turns a program as people write it, its
source code, into the processor's instructions. In 1984 Ken Thompson
showed that a compiler could be altered to insert a weakness into the
programs it builds, and into new copies of itself, so that the weakness
appears in no source code.

Software from a known source, checked against a published hash or
signature, reduces what you must take on trust. Source code you can
read, and build again with other tools to the same result, reduces it
further.

---

## Simple is safer

A simple system is easier to secure, because its owner can know what
it runs, what it accepts from outside and what it stores. Each measure
in this unit is easier to apply to a small machine you understand, and
its effect is easier to check.

```check
(choice
  (ask "Which machine is easier to make secure?")
  (right "One that runs a few programs its owner knows, offering one service")
  (wrong "One with many services, each with the latest features"
    "Each service is a possible way in, and each feature more code to hide weaknesses. More to know about is more to defend.")
  (hint "Which owner can say exactly what their machine runs and accepts?"))
```

So: quantum computers threaten public keys, not a 256-bit symmetric key;
encrypted data is deleted by destroying its key; and small, careful
programs are the safest.
