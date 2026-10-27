# Passwords and keys

```lesson
(objective "Judge how strong a password is by counting the possibilities, and say how a system stores passwords and keys safely.")
(minutes 20)
(keywords "passphrase" "salt (of a password)" "cryptographic key")
```

A password is strong when it cannot be guessed, even by a program that
tries a billion possibilities a second. So what matters is how many
possibilities there are.

A password of 8 lower-case letters, each chosen at random, has 26
choices for each letter: $26^8$ possibilities in all.

```sources
(source "Timeless Computing (docs/tc.md), Security: Passwords and Keys")
```

```check
(number
  (ask "A program tries a billion (" (math "10^9") ") passwords a second. About how many seconds does it take to try all " (math "26^8") " passwords of 8 random lower-case letters?")
  (answer 209) (within 1) (from "26^8 / 10^9") (unit "s")
  (hint "26 to the power 8 is about 209 billion.")
  (hint "209 billion divided by a billion a second."))
```

---

## Length beats symbols

Move the controls. Each extra letter multiplies the possibilities by
the size of the set it is chosen from, so **length** counts for more
than the set. A **passphrase** is a password made of several unrelated
words, each picked at random from a long list.

```figure
(define set (choice 'set '("letters" "all keys" "words") "From"))
(define size (list-ref '(26 95 2048) set))
(define n (slider 'n 1 12 8 "How many" 1))
(define b (* n (/ (log size) (log 2))))
(text 160 30 (string-append (fmt n 0) " chosen at random from " (fmt size 0)) 'centre 'body)
(text 120 66 "possibilities:" 'right 'small)
(math 170 66 (string-append "2^" (fmt b (if (= set 2) 0 1))) 'ink)
(frame 10 84 300 16 'ink 1)
(rect 10 84 (* 300 (/ (min b 132) 132)) 16 'warm)
(line 301 80 301 104 'ink 2)
(text 300 118 "128-bit key" 'right 'tiny)
(text 160 150 "time to try them all, at a billion a second:" 'centre 'small)
(sp-time b 178)
```

```check
(choice
  (ask "Which is stronger: 8 characters chosen at random from all 95 printable keyboard characters, or 5 words chosen at random from a list of 2048?")
  (right "The 5 words" (true "2048^5 > 95^8"))
  (wrong "The 8 characters" (false "95^8 > 2048^5")
    "Each word is one of 2048, as much as 11 random bits, while a character is one of 95, under 7 bits. 5 words give 2 to the power 55 possibilities; 8 characters only about 2 to the power 52.6.")
  (hint "Set the controls to each and compare the powers of 2."))
```

---

```check
(number
  (ask "A list holds 2048 words, which is " (math "2^11") ". A passphrase is 6 words chosen from it at random. It has " (math "2^n") " possibilities. What is n?")
  (answer 66) (from "11 * 6")
  (wrong-if 17 "That adds 11 and 6. Each word multiplies the possibilities by 2 to the power 11, so 11 is added six times.")
  (wrong-if 12288 "That is 2048 times 6. Multiplying the possibilities adds the powers of 2: 6 words of 11 bits each.")
  (hint "Each word multiplies the count by " (math "2^11") ".")
  (hint "Multiplying powers of 2 adds the exponents: 11, six times."))
```

---

## One password per service

Each service should have its own password. If one service leaks its
passwords, and you used the same one elsewhere, every other place opens
with it too.

```check
(choice
  (ask "A website you used once is broken into, and its passwords are published. You used that password for your email too. What should you do?")
  (right "Change the email password, to one used nowhere else")
  (wrong "Nothing: the email service was not broken into"
    "Attackers try leaked passwords on other services, because people reuse them. Your email now opens with a password the world can read.")
  (hint "What would someone holding the leaked list try next?"))
```

---

## Storing passwords

A well-made system never stores your password. It stores a hash of it,
made by a hash function that is **deliberately slow**, together with a
**salt**: a random value, different for each password. To check a
password, it hashes what you typed with the same salt and compares.

- Someone who copies the stored record must spend that slow effort on
  every guess.
- The salt stops one guess being tested against many records at once:
  the same password with two salts gives two different hashes.
- The system can also wait after each wrong try.

```check
(choice
  (ask "Two people choose the same password. The system stores each with its own salt. Are their stored hashes the same?")
  (right "No: the salts differ, so the hashes differ")
  (wrong "Yes: the same password gives the same hash"
    "The hash is made from the password together with the salt. Different salts give completely different hashes.")
  (hint "What goes into the hash function besides the password?"))
```

---

## On Zeitlos

Zeitlos stores its one password this way: a hash with a 16-byte salt,
slowed until one check takes about half a second. Wrong guesses also
make it wait, longer and longer: after 10 wrong tries in a row, a minute
before each next check ([security](docs:security)).

```sources
(source "Zeitlos docs: security.md, The password; Guessing is slow, everywhere at once")
```

```check
(number
  (ask "After 10 wrong tries, Zeitlos waits 60 seconds before each check. About how many guesses can someone make in an hour from then on?")
  (answer 60) (within 1) (from "3600 / 60")
  (wrong-if 3600 "That is one guess a second. Each guess now waits a minute.")
  (hint "An hour is 3600 seconds, and each guess takes a minute."))
```

---

## Keys

**Cryptographic keys** are long random numbers, such as 256 bits, far
too long to remember. They are kept in files or on devices, and
protected as carefully as the data they protect.

A key is also copied as carefully as data: an encrypted file whose key
is lost is lost with it.

```check
(choice
  (ask "You encrypt your files, and keep the only copy of the key on the same card. The card fails. What have you lost?")
  (right "The files, even if an encrypted copy of them survives elsewhere")
  (wrong "Nothing, if you have an encrypted copy of the files"
    "The encrypted copy cannot be read without the key. A key needs copies kept as carefully as the data's.")
  (hint "Can an encrypted copy be read without its key?"))
```

So: count the possibilities, prefer length, use one password per
place, and keep keys as carefully as what they lock.
