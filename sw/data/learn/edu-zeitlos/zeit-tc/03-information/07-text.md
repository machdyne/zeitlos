# Text

```lesson
(objective "Say how text is stored as numbers, and find a character's ASCII code.")
(minutes 20)
(keywords "character (of text)" "ASCII" "control code" "font" "glyph" "Latin-9" "Unicode" "UTF-8" "encoding")
```

Text is stored by giving each **character** (a letter, digit, space or
punctuation mark) a number. The best-known scheme is **ASCII**, from the
1960s. It gives the numbers 0 to 127 to the English letters, the
digits, punctuation and a few codes that control devices. Capital A is
65, small a is 97, and the digit 0 is 48.

Move the slider through the printable ASCII characters.

```figure
(define c (slider 'c 32 126 65 "Code" 1))
(define (h d) (substring "0123456789ABCDEF" d (+ d 1)))
(text 160 80 (if (= c 32) "(space)" (string (integer->char c))) 'centre 40)
(bit-boxes 32 110 8 c 32 #f #t)
(text 160 180 (string-append "decimal " (fmt c 0) "    hex 0x" (h (floor (/ c 16))) (h (modulo c 16))) 'centre 'body 'bold)
(caption "One character, one byte.")
```

```sources
(source "Timeless Computing (docs/tc.md), Information: Text")
```

```check
(number
  (ask "Capital A is 65 and the letters follow in order. What is the code of capital C?")
  (answer 67) (from "65 + 2")
  (wrong-if 3 "3 is C's place in the alphabet. In ASCII, A starts at 65, so C is 65 + 2.")
  (wrong-if 99 "99 is small c. Small letters start at 97; capitals at 65.")
  (hint "A is 65, B is 66."))
```

---

## A digit is not a number

The digit 7 in a text is stored as its code, 55, not as the number 7.
So the text "42" is two bytes, 52 and 50, while the number 42 is one
byte holding 42. A program must change one into the other on purpose.

```check
(number
  (ask "The digit 0 is 48. What is the code of the digit 9?")
  (answer 57) (from "48 + 9")
  (wrong-if 9 "9 is the number. As a character in text, the digit 9 has its own code, 48 + 9.")
  (hint "The digits come in order after 0, which is 48."))
```

---

## Control codes

Codes 0 to 31, and 127, are not printed. They are **control codes**,
which tell a screen or printer what to do: 10 (line feed) moves to a
new line, 13 (carriage return) to the start of the line, 8 is
backspace and 27 is escape.

Systems that follow Unix, an operating system from 1969, end each line
of a text with a line feed; MS-DOS and Windows use carriage return then
line feed. Files moved between them can still show this difference.

```check
(choice
  (ask "Which code ends each line in a Unix-style text file?")
  (right "10, line feed")
  (wrong "13, carriage return"
    "Carriage return alone goes back to the start of the same line. Unix-style files use line feed, 10.")
  (wrong "32"
    "32 is the space, a printable character. Line ends are control codes, below 32.")
  (hint "Which code moves to a new line?"))
```

---

## Codes and shapes

A character's number is not its look. A **font** gives each character
a **glyph**, the shape that is drawn, so the same text can be shown in
many fonts. A computer with no glyph for a character draws something
else in its place, often a box or a question mark.

```check
(choice
  (ask "The same file is shown in two different fonts. What changes?")
  (right "Only the glyphs: the codes stay the same")
  (wrong "The codes of the characters"
    "The file holds codes, and they do not change. The font only decides what shape each code is drawn as.")
  (hint "Is the font part of the file's bytes?"))
```

---

## Beyond English

ASCII covers only English. **Latin-9** (ISO 8859-15) uses codes 160
to 255 for accented letters and the euro sign, enough for most Western
European languages; Zeitlos's screen fonts draw it.

**Unicode** gives a number to every character of every writing system,
with over a million possible codes. It is usually stored as **UTF-8**:
ASCII characters stay single bytes with their old codes, and every
other character takes two to four bytes, each with its top bit set.
Zeitlos sends text as UTF-8 everywhere.

```sources
(source "Zeitlos docs: basic_app.md (Latin-9 screen, UTF-8 between apps)")
```

```check
(number
  (ask "In UTF-8, how many bytes does the letter A take?")
  (answer 1)
  (wrong-if 2 "Characters outside ASCII take two to four bytes. A is in ASCII, and UTF-8 keeps every ASCII character as one byte.")
  (wrong-if 4 "Four bytes is the most a UTF-8 character can take. ASCII characters, such as A, keep a single byte.")
  (hint "Is A an ASCII character?"))
```

---

## The wrong encoding

A way of storing characters as bytes is an **encoding**. Text read
with the wrong encoding comes out garbled. In UTF-8, e with an acute
accent is the two bytes 0xC3 0xA9; read as Latin-9, those are two
other characters, an A with a tilde and a copyright sign. The bytes
are not damaged: read with the right encoding, the text is fine again.
A plain text file does not say its encoding, so keep text in UTF-8.

```check
(choice
  (ask "A file shows strange pairs of characters where accented letters should be. What is the likely cause?")
  (right "It is UTF-8 being read as another encoding")
  (wrong "The file is damaged"
    "The bytes are probably fine. Two-byte UTF-8 letters read one byte at a time in another encoding look like pairs of odd characters.")
  (wrong "The font has no accents"
    "A missing glyph shows as one box or question mark, not as a pair of other characters.")
  (hint "Accented letters take two bytes in UTF-8."))
```

So: each character is a number, ASCII and UTF-8 say which, and a font
decides how each one looks.
