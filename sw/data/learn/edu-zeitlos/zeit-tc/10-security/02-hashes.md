# Cryptography and hashes

```lesson
(objective "Name the three things cryptography provides, and say what a hash function does and what it is used for.")
(minutes 15)
(keywords "cryptography" "confidentiality" "integrity" "authentication" "hash function" "hash" "SHA-256")
```

**Cryptography** is the use of mathematics to protect information. It
provides three things:

- **Confidentiality**: only the intended receiver can read a message.
- **Integrity**: any change to the message is detected.
- **Authentication**: the receiver can be certain who sent it.

They are different. A locked box keeps others from reading what is
inside, but it does not tell you who locked it.

```sources
(source "Timeless Computing (docs/tc.md), Security: What Cryptography Provides")
```

```check
(choice
  (ask "Your bank's message says \"pay 10\", and someone on the way changes it to \"pay 1000\". Which property would have caught that?")
  (right "Integrity")
  (wrong "Confidentiality"
    "Confidentiality stops others reading the message. Someone may still change a message they cannot read. Detecting a change is integrity.")
  (wrong "Authentication"
    "Authentication tells you who sent the message. Here the question is whether it was changed on the way, which is integrity.")
  (hint "Nobody needed to read it. Something about it changed."))
```

---

## No secret methods

Modern cryptography is public. Its methods are published, studied by
others for years before they are trusted, and made into standards.
Only the keys are secret.

A method whose safety depends on its design being secret cannot be
checked by anyone else. It usually fails once the design becomes known,
and designs do become known.

```check
(choice
  (ask "A company sells a lock for messages and will not say how it works, \"for security\". What should you think?")
  (right "Nobody outside can have checked it, so it should not be trusted")
  (wrong "It is safer, because attackers do not know the method"
    "Secret designs leak, and then whatever their safety rested on is gone. A good method stays safe when everyone knows it, as long as the key is secret.")
  (hint "Who has had the chance to look for its weaknesses?"))
```

---

## Hash functions

A **cryptographic hash function** computes a short value of a fixed
size from data of any length. The value is the **hash**, or digest.
**SHA-256**, a widely used one, gives 256 bits, usually written as 64
hex digits.

A good hash function has three properties:

1. The same data always gives the same hash.
2. Any change to the data, even of one bit, gives a completely
   different hash.
3. Nobody can in practice find two different pieces of data with the
   same hash, or data that gives a hash chosen in advance.

```sources
(source "Timeless Computing (docs/tc.md), Security: Hash Functions")
```

```check
(number
  (ask "A SHA-256 hash is 256 bits. Each hex digit stands for 4 bits. How many hex digits is it written with?")
  (answer 64) (from "256 / 4")
  (wrong-if 32 "32 is the number of bytes: 8 bits each. A hex digit holds 4 bits, half a byte.")
  (wrong-if 256 "That counts bits. Each hex digit holds 4 of them.")
  (hint "Divide the bits by the bits in one hex digit."))
```

---

## A fingerprint for data

Because a change of one bit changes the whole hash, a hash serves as a
fingerprint:

- A copy of a file is checked by hashing it and comparing the result
  with the original's hash.
- A program downloaded from elsewhere is checked against the hash its
  author published.
- Passwords are stored as hashes (the next lesson).

```check
(choice
  (ask "You copy a large file and hash both. The hashes are the same. What does that tell you?")
  (right "The copy is the same as the original, bit for bit")
  (wrong "Only that the two files are the same size"
    "Files of the same size almost always have different hashes. Matching hashes mean matching contents.")
  (wrong "Nothing: two files can easily share a hash"
    "With a good hash function, nobody can in practice find two different files with the same hash. Matching hashes are taken as matching data.")
  (hint "What happens to the hash if even one bit differs?"))
```

---

```check
(choice
  (ask "Two files' SHA-256 hashes differ in only their last hex digit. What can you say about the files?")
  (right "They are different files")
  (wrong "They are almost the same: only a little differs"
    "A hash does not measure how much changed. A single changed bit gives a completely different hash, so similar hashes say nothing about similar data.")
  (hint "Is a hash like a summary, where similar data gives similar results?"))
```

---

```check
(choice
  (ask "You know a file's hash. Can you work out the file from it?")
  (right "No: a good hash function cannot be run backwards")
  (wrong "Yes: the hash is a compressed copy of the file"
    "A hash is 256 bits whatever the file's size, so it cannot hold the file. It only identifies it.")
  (hint "Could a whole film fit in 64 hex digits?"))
```

So: cryptography gives confidentiality, integrity and authentication,
and a hash is a short fingerprint that changes completely with any
change to the data.
