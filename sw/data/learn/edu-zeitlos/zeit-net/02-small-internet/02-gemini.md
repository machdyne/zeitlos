# Gemini

```lesson
(objective "Read a Gemini capsule in web, answer its questions, and say how web knows it is talking to the same capsule as before.")
(minutes 20)
(keywords "Gemini" "capsule (Gemini)" "gemtext" "fingerprint (of a key)" "trust on first use")
```

This lesson is about Gemini, the newer protocol of the small internet.

**Gemini** is like Gopher in spirit: one request, one answer, mostly
text. But every Gemini connection is encrypted with TLS, and its pages
are documents with links in them, not menus. A Gemini server is called
a **capsule**. Its URLs start with `gemini://`.

Pages are written in **gemtext**, a plain text format with a few kinds
of line:

| Line | Is |
|---|---|
| # Title | a heading (##, ### smaller ones) |
| => URL words | a link, shown by web as => words |
| * item | a list item |
| > words | a quotation |
| any other line | text |

```sources
(source "Zeitlos docs: gopher_gemini.md, Gemini documents")
```

```check
(choice
  (ask "In gemtext, which line is a link?")
  (right "=> gemini://example.org/notes.gmi My notes")
  (wrong "# My notes"
    "A line starting with # is a heading. Links start with =>.")
  (wrong "* My notes"
    "A line starting with * is a list item. Links start with =>.")
  (hint "It starts with an arrow made of two characters."))
```

---

## Questions from a capsule

A capsule can ask you something: a word to search for, say. Then the
address field shows the URL with `?` after it, and the capsule's
question appears on the status line. Type your answer after the `?` and
press **Enter**.

```check
(choice
  (ask "A capsule asks a question. Where do you type your answer?")
  (right "In the address field, after the ?")
  (wrong "In the page, under the question"
    "Gemini pages have no boxes to type in. The answer goes into the URL itself, after the ?.")
  (wrong "In a separate window that opens"
    "No window opens: web puts the cursor in the address field, after a ?.")
  (hint "Look at the address field: it has a new ? at its end."))
```

---

## Whose key?

On the web, a certificate authority vouches for a site's public key.
Almost every capsule signs its own certificate instead: nobody vouches
for it. So how can `web` know it is talking to the real capsule?

It remembers. The first time you visit a capsule, `web` keeps the
**fingerprint** of its public key: a short number worked out from the
key, always the same for the same key and, in practice, different for
any other. The
fingerprints are kept in `/data/web/pins.txt`. On every later visit the
capsule must show the same key, or `web` refuses to connect. This is
**trust on first use**.

```figure
(define v (choice 'visit '("first" "same key" "new key") "Visit"))
(zn-machine 8 30 100 50 "Capsule" (if (= v 2) "key B" "key A"))
(frame 170 30 142 80 'ink 1)
(text 241 46 "pins.txt" 'centre 'small 'bold)
(text 180 68 "example.org" 'small)
(text 180 86 (if (= v 0) "(nothing yet)" "key A") 'small)
(arrow 108 55 168 70 'ink 1.5)
(text 160 150
  (cond ((= v 0) "Nothing kept: keep key A.")
        ((= v 1) "Key A again: connect.")
        (else "Key B is not key A: refuse."))
  'centre 'body 'bold)
(caption (cond ((= v 0) "The first visit is trusted.")
               ((= v 1) "A later visit, the same capsule.")
               (else "A later visit: someone else, or a rebuilt capsule.")))
```

```check
(choice
  (ask "You visit a capsule for the second time, and it shows a different key from the first time. What does web do?")
  (right "Refuses to connect, saying the key has changed")
  (wrong "Shows a warning, and lets you go on"
    "web never connects past a failed check: a warning people click through protects nobody. It refuses.")
  (wrong "Keeps the new key and connects"
    "That would let anyone who got between you and the capsule take its place. A changed key is refused.")
  (hint "Look at the figure with Visit set to new key."))
```

---

## When the key really changed

A capsule's owner may rebuild it with a new key. If you know that is
what happened, open `/data/web/pins.txt` in `text`, delete that host's
line, and save. The next visit keeps the new key.

The weak point is the first visit: whoever answers then is the one
remembered. That is the price of having nobody to vouch for a capsule,
and why `web` uses trust on first use only for Gemini, never for https.

```check
(choice
  (ask "What does trust on first use NOT protect?")
  (right "The very first visit to a capsule")
  (wrong "Every later visit"
    "Later visits are exactly what it protects: the key must match the one kept the first time.")
  (wrong "Gemini pages that are long"
    "Length has nothing to do with it. The question is whom you trusted, and when.")
  (hint "Before the first visit, what is there to compare with?"))
```

---

## No clock needed

A capsule's certificate dates are not checked, which is the custom for
certificates that sign themselves. So Gemini works even before `net`
has set the clock, when https pages are still refused.

What `web` cannot do in Gemini: log in to a capsule that wants a
certificate from you, and show pictures.

```check
(choice
  (ask "Just after starting, https pages are refused because the clock is not set. Will a Gemini capsule open?")
  (right "Yes: web does not check a capsule's dates")
  (wrong "No: all encrypted pages need the clock"
    "https needs it, to check that a certificate has not expired. Gemini's pins do not use dates, so they need no clock.")
  (hint "What does a pin compare: dates, or keys?"))
```

---

```check
(text
  (ask "What is a Gemini server called?")
  (accept "capsule") (accept "a capsule")
  (wrong-if "site" "That is the web's word. Gemini has its own.")
  (hint "Like something sent into space."))
```

So: Gemini is encrypted gemtext from capsules, and `web` trusts a
capsule's key the first time and insists on it afterwards.
