# The problem

```lesson
(objective "Say what makes modern computers hard to own, and what timeless computing aims for instead.")
(minutes 15)
(keywords "hardware (of a computer)" "software (of a computer)" "timeless computing (idea)" "layer (of software)")
```

You will see why modern computers can be hard to own, and what this
course, and the book it follows, asks of a computer instead.

A computer has two sides. Its **hardware** is the machine itself: the
parts you can touch. Its **software** is the programs it runs: the
instructions that tell the hardware what to do.

For fifty years computers have become faster, cheaper and able to do
more. Over the same years they have become harder to understand, to
keep safe from attack and to keep working.

```sources
(source "Timeless Computing (docs/tc.md), chapter 1, Timeless Computing")
```

```check
(choice
  (ask "A program that lets you write letters. Is it hardware or software?")
  (right "Software")
  (wrong "Hardware"
    "Hardware is what you can touch: the keyboard, the screen, the box. A program is a set of instructions the hardware follows, so it is software.")
  (hint "Can you pick it up and hold it?"))
```

---

## Layers

The software of a modern computer is large, and built in **layers**: a
program sits on top of other programs, which sit on others in turn.
When you press a key, the press passes through layer after layer before
anything appears. Each layer adds a little delay.

So in some ways a modern computer can feel slower than a much older
one, even though its hardware is far faster.

```check
(choice
  (ask "A new computer takes a moment to show a letter after you press its key. An old one shows it at once. What is the likely reason?")
  (right "The press passes through more layers of software")
  (wrong "The new computer's hardware is slower"
    "The new hardware is far faster. The delay comes from the many layers of software the key press has to pass through.")
  (wrong "Keyboards have become worse"
    "The keyboard sends the press at once in both. What differs is how much software the press goes through before the letter appears.")
  (hint "What has grown on a modern computer: the speed of its parts, or the amount of software?"))
```

---

## Too big to read

The software of a typical computer is far too large for any one person
to read. Its owner depends on the work of thousands of people. With so
much of it, mistakes and weaknesses that an attacker could use are hard
to find.

```check
(choice
  (ask "Why are mistakes hard to find in the software of a typical modern computer?")
  (right "There is far too much of it for anyone to read")
  (wrong "Its makers hide them on purpose"
    "Nobody needs to hide them. The software is the work of thousands of people, and there is simply too much of it for any one person to read and check.")
  (wrong "Software cannot have mistakes"
    "Software is written by people, and people make mistakes. The more there is of it, the harder they are to find.")
  (hint "Think about how big it is."))
```

---

## Replaced too soon

Hardware is usually replaced long before it wears out. Software grows
to use the extra capacity of newer hardware, and older hardware that
still works perfectly can no longer run the newest software.

So the owner often ends up **serving the computer**: installing
updates, replacing hardware that still works, and learning again
software that has changed. The computer was meant to be a tool that
serves its owner.

```check
(choice
  (ask "A ten-year-old laptop still works perfectly, but cannot run the new version of a program. Why?")
  (right "The software has grown to need newer hardware")
  (wrong "The laptop has worn out"
    "It still works perfectly, so it has not worn out. The program has grown to use what newer hardware can do.")
  (wrong "Laptops are made to stop after ten years"
    "Nothing in the laptop has stopped. It is the software that has changed, growing to use newer, larger hardware.")
  (hint "Which side changed: the old laptop, or the program?"))
```

---

## Timeless computing

**Timeless computing** is an approach that puts three things first:
lasting a long time, simplicity, and understanding. Its aim is a
computer that stays useful for decades, whose owner understands how it
works and can change it as needs change.

```sources
(source "Timeless Computing (docs/tc.md), chapter 1, The Problem")
```

```check
(choice
  (ask "Which computer fits the aim of timeless computing best?")
  (right "One that stays useful for decades, and that its owner understands")
  (wrong "The fastest computer you can buy this year"
    "Speed is not the aim. A fast computer that is replaced in a few years, and that nobody can fully understand, is the problem the lesson describes.")
  (wrong "One that gets a new version of its software every week"
    "Constant updates are part of the problem: the owner ends up serving the computer. Timeless computing wants a machine that stays the same and stays useful.")
  (hint "The three things it puts first: lasting, simplicity, understanding."))
```

---

```check
(choice
  (ask "Which of these is an example of the owner serving the computer?")
  (right "Replacing a printer that still works because new software no longer supports it")
  (wrong "Writing a letter with a text editor"
    "Here the computer is serving its owner, as a tool. Serving the computer is work done only to keep it going: updates, replacing working hardware, relearning.")
  (wrong "Saving your work so that it is kept safe"
    "Saving your work is using the computer as a tool for your own purpose. Serving it means doing work only because the computer demands it.")
  (hint "Which one is work that only exists because of the computer's own changes?"))
```

So modern computers are fast but large, layered and short-lived, and
timeless computing aims for one that lasts and that you can understand.
