# Threats

```lesson
(objective "Say what is to be protected on a computer, from whom, and why holding the machine or a copy of its data matters most.")
(minutes 15)
(keywords "threat model" "encryption" "offline copy" "ransomware")
```

A computer is **secure** when it does what its owner intends and
nothing else. Its data is read only by those meant to read it, changed
only by those meant to change it, and there when it is needed.

So there are three ways to fail: someone reads what they should not,
someone changes what they should not, or the data is gone when you need
it.

```sources
(source "Timeless Computing (docs/tc.md), Security")
```

```check
(choice
  (ask "A power cut corrupts the only copy of your diary. Nobody else ever saw it. Was this a security failure?")
  (right "Yes: the data was not there when it was needed")
  (wrong "No: nobody read or changed it"
    "Being secure also means the data is available when you need it. Losing it is a failure even when no stranger is involved.")
  (wrong "No: security is only about attackers"
    "Security is about the machine doing what its owner intends. A lost diary is a failure whatever caused it.")
  (hint "There are three ways to fail. Is one of them about the data being there at all?"))
```

---

## A threat model

You cannot defend against everything at once, and you need not. A
**threat model** says what is to be protected, from whom, and at what
cost.

- A notebook of private writing needs protection from someone who picks
  up the machine.
- A server on the Internet needs protection from strangers on the
  network who try every address.
- A messaging device may need protection from someone who records
  everything sent, and can afford to wait.

Measures that suit one are wasted effort, or not enough, for another.

```check
(choice
  (ask "Which question does a threat model answer first?")
  (right "What am I protecting, and from whom?")
  (wrong "Which lock for my data is the strongest?"
    "The strongest lock is no use against a threat you did not think of, such as a stolen machine. You choose defences after you know what you face.")
  (wrong "How can I protect against everything?"
    "Nobody can. A threat model is how you choose which threats matter, so that your effort goes where it helps.")
  (hint "Before choosing a lock, you need to know what is behind the door, and who might try it."))
```

---

## Most failures are ordinary

Most security failures are not codes being broken. They come from:

- lost or stolen equipment;
- weak or reused passwords;
- programs that trust whatever input they are given;
- software from an unknown source;
- no copy when something is deleted.

---

## Physical access

Whoever holds a machine can do almost anything with it. They can read
its storage card in another computer, replace its flash, attach a
device to its pins, or take it away.

A password that locks the screen protects the running session from
someone at the keyboard. It does not protect the data on a card that
can be pulled out. Zeitlos says this plainly: its password locks the
screen, but the card is in an ordinary format that any computer can
read ([security](docs:security)).

```sources
(source "Timeless Computing (docs/tc.md), Security: Physical Access")
(source "Zeitlos docs: security.md, What it protects, and what it does not")
```

```check
(choice
  (ask "Your Zeitlos machine has a screen lock. A thief takes the storage card out of it. What can the thief read?")
  (right "Every file on the card, in any other computer")
  (wrong "Nothing, because the screen is locked"
    "The lock is a program running on the machine. Once the card is in another computer, that program is not running, and the card is in an ordinary format.")
  (wrong "Only the files that were open"
    "The card holds all your files whether or not they were open. The screen lock never touched them.")
  (hint "Is the lock part of the card, or part of the running machine?"))
```

---

## Keep it encrypted

Data that must stay private on a machine that could be lost is kept
encrypted. **Encryption** turns data into a form that can be read only
with a secret number, the key. The key is not stored beside the data:
a locked box with its key taped to the lid protects nothing.

Equipment that matters is kept where others cannot reach it, like any
other valuable.

```check
(choice
  (ask "A laptop's disk is encrypted, and the key is kept in a file on the same disk, unprotected. A thief takes the laptop. Is the data safe?")
  (right "No: the thief has the key as well as the data")
  (wrong "Yes: the data is encrypted"
    "Encryption is only as safe as its key. Here the key travels with the data, so the thief has both.")
  (hint "Where is the key?"))
```

---

## Copies

The last defence against every kind of loss is a copy. An **offline
copy** is one kept on a drive that is not connected, such as a USB
stick in a drawer.

Offline copies are the answer to **ransomware**: software that deletes
or encrypts your files and asks for payment to restore them. It can
reach everything the machine can reach, including a copy on a drive
that is plugged in. It cannot reach the stick in the drawer.

Zeitlos has an app for dated copies on a second drive, `backup`
([backup](docs:backup)).

```sources
(source "Timeless Computing (docs/tc.md), Security: Copies")
```

```check
(choice
  (ask "Ransomware encrypts every file the machine can reach. Which copy survives?")
  (right "A copy on a stick that was unplugged and put away")
  (wrong "A copy in another folder of the same card"
    "The ransomware can reach every folder the machine can reach. A second folder on the same card is encrypted along with the first.")
  (wrong "A copy on a stick that is always plugged in"
    "A drive that is connected can be reached like any other. Only a copy that is not connected is out of reach.")
  (hint "What can the program on the machine not reach at all?"))
```

---

```check
(choice
  (ask "Which of these is the most common cause of security failures?")
  (right "Lost equipment, weak passwords and missing copies")
  (wrong "Encryption methods being broken"
    "Breaking modern encryption is rare. Most failures are ordinary: a lost machine, a reused password, a program that trusts its input.")
  (hint "Think of the list of ordinary failures."))
```

So: decide what you protect and from whom, and remember that whoever
holds the machine, or has no copy, has lost the most.
