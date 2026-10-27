# Randomness and the one-time pad

```lesson
(objective "Say where a computer gets unpredictable numbers, and how the one-time pad encrypts with XOR and why its key must never be reused.")
(minutes 20)
(keywords "true random number generator" "TRNG" "pseudo-random number generator" "seed (of a generator)" "one-time pad" "information-theoretic security")
```

Keys, salts and nonces must be unpredictable. But a computer is made
to do the same thing every time. So it gathers unpredictability from
physical noise, such as the jitter in the speed of a free-running
oscillator: a circuit that switches back and forth as fast as it can.
A **true random number generator** (**TRNG**) measures that noise.

Zeitlos's TRNG, on the boards that have one, samples eight such
oscillators ([trng](docs:trng)).

```sources
(source "Timeless Computing (docs/tc.md), Security: Random Numbers")
(source "Zeitlos docs: trng.md")
```

---

## Stretching the noise

Physical noise is slow and imperfect. So systems use it to **seed** a
**cryptographically secure pseudo-random number generator**: a
calculation that starts from a secret value, the seed, and produces an
unlimited stream of numbers that nobody can predict without knowing
the seed. Zeitlos builds its generator on ChaCha20.

A weak generator makes keys predictable, and a connection made with
such keys shows no outward sign of it. That is why the source of
randomness is among the most important parts of a secure system.

```check
(choice
  (ask "A program makes its keys from the time of day, in seconds. What is wrong?")
  (right "An attacker can try every second around the time, and so find the key")
  (wrong "Nothing: nobody knows the exact second"
    "There are only 86,400 seconds in a day. Trying them all takes a computer a moment, so the key is predictable.")
  (wrong "The connection would fail, so you would notice"
    "A predictable key works perfectly. Nothing looks wrong at either end; only the attacker knows.")
  (hint "How many different seconds are there in a day, and how fast can a computer try them?"))
```

---

## The one-time pad

One method of encryption cannot be broken, however much computing
power an attacker has. In the **one-time pad**, each bit of the message
is combined by XOR (exclusive or: 1 when the two bits differ) with one
bit of a key that is truly random and as long as the message.

The receiver, holding the same key, applies XOR with it again, which
gives back the message. Move the sliders.

```figure
(define m (+ 65 (slider 'm 0 25 7 "Letter" 1)))
(define k (slider 'k 0 255 181 "Key" 1))
(define e (sp-xor m k))
(sp-row 20 (string-append "message " (substring "ABCDEFGHIJKLMNOPQRSTUVWXYZ" (- m 65) (- m 64))) m)
(sp-row 62 "key" k)
(line 100 98 292 98 'ink 1)
(sp-row 106 "encrypted" e)
(sp-row 148 "key" k)
(line 100 184 292 184 'ink 1)
(sp-row 192 "decrypted" (sp-xor e k))
```

```sources
(source "Timeless Computing (docs/tc.md), Security: The One-Time Pad")
```

```check
(text
  (ask "Message bits 0110, key bits 1100. What are the encrypted bits?")
  (accept "1010")
  (wrong-if "0100" "That is AND: 1 only where both are 1. XOR gives 1 where the bits differ.")
  (wrong-if "1110" "That is OR. XOR gives 0 where both bits are 1.")
  (hint "Go bit by bit: 1 where the two bits differ, 0 where they are the same."))
```

---

## Why it cannot be broken

Every key is equally likely, so every message of the same length is
equally consistent with the encrypted bits. The encrypted text reveals
nothing but its length. This is **information-theoretic security**: it
rests on the randomness of the key, not on any calculation being hard.

```check
(choice
  (ask "An attacker with unlimited computing power tries every possible key on a one-time-pad message of 5 letters. What do they learn?")
  (right "Only that it is 5 letters long: every 5-letter message comes out of some key")
  (wrong "The message, after trying enough keys"
    "Trying every key gives every possible message, \"HELLO\" and \"PIZZA\" alike, each from one key. Nothing says which is right.")
  (hint "Pick any 5-letter message you like. Is there a key that turns the encrypted bits into it?"))
```

---

## The strict conditions

The guarantee holds only if the key is truly random, as long as all
the messages it protects, kept secret, and **never used twice**. It
must also reach the receiver in advance, by some other way than the
channel it protects: for example, a memory card handed over in person.

If the same key is used twice, XOR of the two encrypted texts cancels
the key, leaving the XOR of the two messages, from which both can often
be recovered.

```check
(choice
  (ask "Two messages were encrypted with the same key. An attacker XORs the two encrypted texts together. What do they get?")
  (right "The XOR of the two messages: the key has cancelled out")
  (wrong "Random bits, which tell them nothing"
    "Each encrypted text is a message XOR the key. XOR the two, and the key appears twice and cancels: what is left depends only on the messages.")
  (wrong "The key"
    "The key cancels: anything XORed with itself is 0. What remains is the two messages combined.")
  (hint "What is the key XOR the key?"))
```

---

```check
(text
  (ask "A one-time pad: the encrypted bits are 1001 and the key is 0011. What was the message?")
  (accept "1010")
  (wrong-if "0001" "That is AND. Decrypting is XOR with the key again: 1 where the bits differ.")
  (wrong-if "1011" "That is OR. With XOR, two 1s give 0.")
  (hint "Decrypt by XOR with the same key: 1 where the two bits differ."))
```

So: good keys come from physical noise, and the one-time pad, with a
truly random key used once, cannot be broken at all.
