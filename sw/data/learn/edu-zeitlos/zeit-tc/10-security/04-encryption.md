# Encryption

```lesson
(objective "Say what symmetric and public-key encryption each do, why both are used, and what a signature proves.")
(minutes 20)
(keywords "symmetric encryption" "AES" "ChaCha20" "authenticated encryption" "nonce" "public-key cryptography" "public key" "private key" "key exchange" "digital signature" "RSA" "certificate (of a public key)" "SSH")
```

**Symmetric encryption** uses one secret key both to encrypt and to
decrypt. Two widely used methods are **AES**, a standard since 2001,
and **ChaCha20**, which is faster in software on processors without
special instructions for AES.

Keys are usually 128 or 256 bits long. A 128-bit key has $2^128$
possible values: so many that trying them all is beyond any computer
that could be built.

```sources
(source "Timeless Computing (docs/tc.md), Security: Symmetric Encryption")
```

```check
(choice
  (ask "A 256-bit key instead of a 128-bit key. How many times as many possible keys?")
  (right "2 to the power 128 times as many" (true "2^256 / 2^128 = 2^128"))
  (wrong "Twice as many" (false "2^256 / 2^128 = 2")
    "Twice the bits is not twice the keys. Each extra bit doubles the count, and there are 128 extra bits: 2 to the power 128 times as many.")
  (wrong "128 times as many" (false "2^256 / 2^128 = 128")
    "Each of the 128 extra bits doubles the count. That is 2 multiplied by itself 128 times, not 128.")
  (hint "Each extra bit doubles the number of keys. How many extra bits are there?"))
```

---

## Secret is not enough

Encryption alone gives confidentiality, but not integrity: someone who
cannot read a message may still be able to change it. So current
methods use **authenticated encryption**: a short tag, computed from
the message and the key, is sent with it, and the receiver checks the
tag before trusting the message.

Each message is also encrypted with a **nonce**, a number that must
never be used twice with the same key. Reusing one can reveal the
messages.

```check
(choice
  (ask "An attacker flips some bits of an encrypted message on its way. With authenticated encryption, what happens?")
  (right "The tag no longer matches, so the receiver rejects the message")
  (wrong "The receiver decrypts it to a slightly different message and believes it"
    "That is what can happen with encryption alone. The tag is there to catch exactly this: a changed message no longer matches it.")
  (wrong "Nothing: the attacker cannot change an encrypted message"
    "Bits on a wire can always be changed. Encryption stops them being read; the tag is what shows they were changed.")
  (hint "What is the tag computed from, and when is it checked?"))
```

---

## The trouble with one key

Symmetric encryption is fast, but both ends must already share the key.
Getting it to them safely is hard: if you could send it safely, you
could have sent the message the same way.

**Public-key cryptography** solves this with a pair of keys. The
**public key** can be given to anyone; the matching **private key** is
kept secret. The private key gives the public key in an instant, but,
as far as is known, getting the private key back from the public one
would take present computers longer than the age of the universe.

A padlock is a fair picture: anyone can snap an open padlock shut, but
only the holder of its key can open it.

```sources
(source "Timeless Computing (docs/tc.md), Security: Public-Key Cryptography")
```

```check
(choice
  (ask "You want strangers to be able to send you secrets. Which key do you publish?")
  (right "Your public key")
  (wrong "Your private key"
    "The private key is the one that opens what was locked with the public key. Publish it, and anyone can read your messages.")
  (wrong "Both, so that anyone can talk to you"
    "With the private key published, there are no secrets left. Only the public key, like an open padlock, is handed out.")
  (hint "In the padlock picture, what do you hand out, and what do you keep?"))
```

---

## Two uses

- **Key exchange.** Two machines that have never met can agree on a
  shared secret over an open network, even while others listen. X25519
  is a common method. The secret becomes the key for fast symmetric
  encryption.
- **Digital signatures.** A message signed with a private key can be
  checked by anyone with the public key, and nobody without the private
  key can forge the signature. Ed25519 is a common method. A signature
  proves who wrote a message, or published a program, and that it has
  not changed since.

Many systems, especially older ones, use **RSA** for both, with much
longer keys.

```check
(choice
  (ask "You download a program signed by its author. The signature checks out with the author's public key. What does that prove?")
  (right "The author signed it, and it has not changed since")
  (wrong "The program has no mistakes in it"
    "A signature says who published the program and that nobody changed it. It says nothing about whether the author made mistakes.")
  (wrong "Nobody else could have read it on the way"
    "A signature is not encryption: anyone can read a signed program. It proves who signed it and that it is unchanged.")
  (hint "Which properties does a signature give: confidentiality, integrity or authentication?"))
```

---

## Whose key is it?

A public key raises a question of trust: does it really belong to the
person or site you think? The web relies on **certificates**: public
keys signed by organizations that browsers are set up to trust. **SSH**, a
way of logging in to another machine over the network, usually asks you to confirm a server's key the first time and remembers
it. Between people, keys can be compared in person, or their hashes
read aloud.

All this takes arithmetic. On a small processor, public-key cryptography
can take seconds a connection; on an FPGA computer, the slow parts can
be given circuits of their own.

```check
(choice
  (ask "Someone on your network hands you their own public key, claiming it is your bank's. What protects you?")
  (right "Checking the key: a certificate, or comparing it with one you already trust")
  (wrong "The encryption: messages to that key are secret"
    "They are secret, but from the wrong person: whoever holds the matching private key can read them. You must know whose key it is.")
  (hint "Encrypting to a key keeps secrets from everyone except whom?"))
```

---

```check
(choice
  (ask "Why do connections use public-key cryptography first, then symmetric encryption for the data?")
  (right "Public keys agree on a shared key; symmetric encryption is much faster for the data")
  (wrong "Symmetric encryption is stronger, so it protects the data"
    "Both are strong. The reason is speed: public-key arithmetic is slow, symmetric encryption fast, so each does what it is good at.")
  (wrong "Public-key cryptography cannot encrypt anything"
    "It can, but slowly. Its great use is to agree on a key with a stranger; the fast symmetric method then carries the data.")
  (hint "Which is slow, and what problem does it solve that the fast one cannot?"))
```

So: symmetric encryption is fast but needs a shared key; public-key
cryptography agrees on that key with strangers and signs what you
publish.
