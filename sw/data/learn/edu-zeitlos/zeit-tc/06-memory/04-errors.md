# Catching errors

```lesson
(objective "Use a parity bit and a checksum to notice a changed bit, and say what stronger checks add.")
(minutes 20)
(keywords "parity bit" "parity" "checksum" "cyclic redundancy check" "CRC" "cryptographic hash" "error-correcting code" "ECC")
```

This lesson is about noticing when stored data has changed.

Storage is not perfect. A worn flash cell, a cosmic ray striking a
memory chip, a write cut short or a bad cable can each change a bit.
The defence is to store a little extra information, so that a change
can be noticed, and sometimes put right.

A **parity bit** says whether a group of bits has an odd or even number
of 1s. With even **parity**, the parity bit is chosen so that the 1s,
counting it, are even. On reading, an odd count means a bit has
changed.

```sources
(source "Timeless Computing (docs/tc.md), Memory and Storage: Errors and How to Catch Them")
```

```check
(number
  (ask "The byte 1011 0010 is stored with even parity. What is its parity bit?")
  (answer 0)
  (wrong-if 1 "The byte has four 1s, already even. The parity bit must keep the count even, so it is 0.")
  (hint "Count the 1s in 1011 0010. Is the count even already?"))
```

---

## Try it

Choose a byte, then change bits as if the storage had failed. The
reader counts the 1s, the parity bit included.

```figure
(define n (slider 'byte 0 255 178 "Byte" 1))
(define f (choice 'flip '("none" "1 bit" "2 bits") "Changed"))
(define p (modulo (cp-ones n 8) 2))
(define m (cond ((= f 0) 0) ((= f 1) 4) (else 36)))
(define (flipped k) (= 1 (modulo (floor (/ m (expt 2 (- 7 k)))) 2)))
(text 16 30 "as read back:" 'small)
(for-range 0 9
  (lambda (k)
    (let* ((x (+ 16 (* k 32) (if (= k 8) 8 0)))
           (b (if (= k 8) p (modulo (floor (/ n (expt 2 (- 7 k)))) 2)))
           (v (if (and (< k 8) (flipped k)) (- 1 b) b)))
      (frame x 42 28 28 'ink (if (and (< k 8) (flipped k)) 3 1))
      (text (+ x 14) 62 (fmt v 0) 'centre 'body 'bold))))
(text 284 88 "parity" 'centre 'tiny)
(define (rbit k)
  (let ((b (modulo (floor (/ n (expt 2 (- 7 k)))) 2)))
    (if (flipped k) (- 1 b) b)))
(define cnt (let loop ((k 0) (c p)) (if (= k 8) c (loop (+ k 1) (+ c (rbit k))))))
(text 160 120 (string-append "1s counted, parity bit too: " (fmt cnt 0)) 'centre 'small)
(define now (modulo cnt 2))
(text 160 150 (if (= now 0) "even: looks right" "odd: an error is detected") 'centre 'body 'bold)
(text 160 180 (cond ((= f 0) "Nothing changed.") ((= f 1) "One bit changed, and parity sees it.")
                    (else "Two bits changed: wrong, yet even."))
      'centre 'small)
(caption "Thick boxes: bits that changed")
```

```check
(choice
  (ask "Two bits of a byte change. What does a parity check say?")
  (right "Nothing: two changes leave the count even or odd, as it was")
  (wrong "That two bits are wrong"
    "Parity is one bit: it can only say odd or even. Two changes undo each other's effect on it.")
  (wrong "That one bit is wrong"
    "Each change flips the parity, so two flips bring it back. The check passes, though the byte is wrong.")
  (hint "Each changed bit flips the count between odd and even. Flip twice."))
```

---

## Checksums

A **checksum** is the sum of all the bytes of a block, stored with it.
If the sum of the bytes read back no longer matches, the block has
changed.

```check
(number
  (ask "A block holds the bytes 10, 20 and 30, and its checksum, 60. The 20 is read back as 21. What sum does the reader get?")
  (answer 61) (from "10 + 21 + 30")
  (wrong-if 60 "60 was the stored checksum. Add the bytes as read: 10 + 21 + 30.")
  (hint "Add the three bytes as they were read."))
```

---

```check
(choice
  (ask "The bytes 10, 20, 30 are read back as 20, 10, 30: two swapped. Does the checksum notice?")
  (right "No: the sum is still 60")
  (wrong "Yes: two bytes are wrong"
    "A sum does not care about order. 20 + 10 + 30 is 60, the same as before, so the checksum passes.")
  (hint "Add 20 + 10 + 30."))
```

---

## Stronger checks

- A **cyclic redundancy check** (**CRC**) is a more careful kind of
  checksum that notices almost all accidental damage. Disks, network
  messages and many file formats use one.
- A **cryptographic hash** makes a fingerprint that
  changes completely if even one bit changes, and that cannot in
  practice be forged. It shows a file has not been changed, by accident
  or on purpose.
- An **error-correcting code** (**ECC**) stores enough extra bits not
  just to notice a changed bit, but to find which one it was and put it
  right.

```check
(choice
  (ask "Which of these can put a changed bit right, not just notice it?")
  (right "An error-correcting code")
  (wrong "A parity bit"
    "Parity says a bit changed, not which one. Only a code with more extra bits can point at it.")
  (wrong "A CRC"
    "A CRC notices damage very well, but it does not say which bit to fix.")
  (hint "Which one has correcting in its name?"))
```

---

## Read it back

The most important habit is to read back anything important after
writing it, and compare it with the original. A copy that has not been
checked cannot be trusted as a backup.

```check
(choice
  (ask "You copy a folder of work to a USB stick for safe keeping. What should you do next?")
  (right "Read the copy back and compare it with the original")
  (wrong "Nothing: the copy finished without an error"
    "A write can finish and still be wrong: a weak cell or a bad cable shows up only when you read it back.")
  (wrong "Copy it a second time over the first"
    "A second copy that is not checked is no surer than the first. Reading back is what proves it.")
  (hint "How do you know the bits on the stick are the right ones?"))
```

So: parity and checksums notice many changes, CRCs and cryptographic hashes notice
almost all, ECC can fix them, and reading back proves a copy.
