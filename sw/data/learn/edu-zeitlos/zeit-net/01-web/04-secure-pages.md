# Secure pages and local files

```lesson
(objective "Say what HTTPS protects and what web needs to check a secure page, and open a page stored on your own computer.")
(minutes 20)
(keywords "encryption" "encrypt" "TLS" "private key" "public key" "certificate (TLS)" "certificate authority")
```

This lesson is about what keeps a page private on its way to you.

A page does not travel straight from the server to you. It passes
through many machines on the way: your router, your provider's, and
others. Any of them could read it, or change it.

**Encryption** is scrambling data with a secret number, so that only
someone who knows the secret can turn it back into the data. Turn it on
in the figure to see what a machine on the way would read.

```figure
(define enc (toggle 'enc "Encrypted"))
(zn-machine 6 40 70 50 "You" "")
(zn-machine 244 40 70 50 "Server" "")
(line 76 65 244 65 'ink 2)
(zn-machine 105 140 110 50 "On the way" "")
(line 160 140 160 66 'grey 1)
(text 160 214 (if enc "reads: 7f#Qz!9v@Lk2&w" "reads: password=tulip7") 'centre 'body 'bold)
(caption (if enc "Scrambled: it cannot be read or changed unseen." "Plain: anyone on the way can read it."))
```

```check
(choice
  (ask "What does encryption protect you from?")
  (right "Machines on the way reading or changing what you send and receive")
  (wrong "Pages that need JavaScript"
    "Encryption is about who can read the data on its way. What the page needs to show itself is another matter.")
  (wrong "A slow network"
    "Encryption makes nothing faster: it costs some time, in fact. It keeps the data private on the way.")
  (hint "Look at what the machine on the way reads, with the box ticked and without."))
```

---

## TLS

**HTTPS** is HTTP sent inside **TLS** (Transport Layer Security). TLS
does two things before the page is sent:

1. It checks that the server is really the one named in the URL.
2. It agrees a secret with that server, and **encrypts** everything after
   with it.

Plain `http` does neither: the page travels as it is.

```sources
(source "Timeless Computing (docs/tc.md), Communication: Secure Connections")
(source "Zeitlos docs: tls.md")
```

```check
(choice
  (ask "Which page is protected on its way to you?")
  (right "https://example.org/")
  (wrong "http://example.org/"
    "Plain http sends the page as it is. The s in https means it travels inside TLS, encrypted.")
  (hint "Look for the extra letter."))
```

---

## Keys and certificates

How can a server prove who it is? It holds a **private key**, a long
secret number it never sends. With it, it can answer a challenge in a
way that anyone can check, using the matching **public key**, which
the server shows openly. Only the holder of the private key can make
such an answer.

But whose public key is it? A **certificate** says: this public key
belongs to this host name. A **certificate authority** vouches for it
with its own key: an organisation that browsers trust to check first.

`web` keeps the authorities it trusts in a file on the card,
`/data/web/roots.der`.

```check
(choice
  (ask "What does a certificate say?")
  (right "That a public key belongs to a host name")
  (wrong "The server's private key"
    "The private key is never sent to anyone. A certificate holds the public key and the name it belongs to.")
  (wrong "Which pages on the server are safe"
    "A certificate says nothing about the pages, only who the server is.")
  (hint "It ties two things together: a key and a name."))
```

---

## Refuse, do not warn

If a certificate does not check out (the wrong name, expired,
vouched for by nobody `web` trusts), `web` **refuses** the page and says why.
There is no button to go on anyway: a warning that people click past
protects nobody.

To know whether a certificate has expired, `web` needs the date. `net`
sets the clock from the network when it starts. Until it has, secure
pages are refused; and so they are without `roots.der` on the card.

```sources
(source "Zeitlos docs: tls.md, Certificates are verified; Two things must be on the card")
```

```check
(choice
  (ask "Just after starting, web refuses every https page, but http pages open. What is a likely reason?")
  (right "The clock is not set yet, so certificates cannot be checked")
  (wrong "The network is not working"
    "If the network were down, the http pages would not open either.")
  (wrong "Every https site has a bad certificate"
    "It is unlikely they all do at once. Something web needs to check any certificate is missing.")
  (hint "What does web need, besides roots.der, to tell whether a certificate has expired?"))
```

---

## Pages on your own computer

`web` also opens pages stored on the card, with no network at all.
Type a path that starts with `/` into the address field, such as
`/home/site/start.html`; it becomes the URL
`file:///home/site/start.html`. In `files`, an `.html` file opens in
`web` too. Links in such a page lead to the files beside it.

```check
(text
  (ask "Which scheme does web use for a page stored on the card?")
  (accept "file") (accept "file:") (accept "file://") (accept "file:///")
  (wrong-if "https" "https fetches a page from a server, over the network. A page on the card needs no server.")
  (wrong-if "http" "http fetches a page from a server. A page on the card has a scheme of its own.")
  (hint "Typing /home/site/start.html becomes ...:///home/site/start.html."))
```

---

```check
(choice
  (ask "Which of these does a server never send to anyone?")
  (right "Its private key")
  (wrong "Its public key"
    "The public key is shown openly, inside the certificate: anyone may have it.")
  (wrong "Its certificate"
    "The server sends its certificate to every visitor, so they can check who it is.")
  (hint "One of the two keys is secret."))
```

So: HTTPS is HTTP inside TLS, which checks the server's certificate and
encrypts the page, and `web` refuses a page it cannot check.
