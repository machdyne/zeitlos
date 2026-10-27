# Web addresses

```lesson
(objective "Read the parts of a URL, open a page in web, follow links and go back.")
(minutes 20)
(keywords "URL" "scheme (of a URL)" "path (of a URL)" "HTTP" "HTTPS")
```

This lesson is about opening pages in `web`, Zeitlos's web browser.

Every page has an address of its own, its **URL** (Uniform Resource
Locator). A URL has three main parts:

- the **scheme**, before `://`: which protocol to use, such as
  **HTTP** or its secure form, **HTTPS**;
- the host name: which server to ask;
- the **path**, from the next `/` on: which page on that server.

Choose a part to see where it is.

```figure
(define p (choice 'part '("scheme" "host name" "path") "Part"))
(define x0 30)
(text x0 100 "https://example.org/news.html" 'body)
(text 160 40 "A URL, in three parts" 'centre 'small)
(cond
  ((= p 0) (zn-under x0 100 0 5 "the scheme"))
  ((= p 1) (zn-under x0 100 8 19 "the host name"))
  (else    (zn-under x0 100 19 29 "the path")))
```

```check
(text
  (ask "In the URL https://example.org/news.html, what is the host name?")
  (accept "example.org")
  (wrong-if "https" "That is the scheme: the protocol. The host name comes after the two slashes.")
  (wrong-if "news.html" "That is the path: the page on the server. The server's name comes before it.")
  (wrong-if "example" "Type the whole name, with its dot and what comes after: the server's name is all of it.")
  (hint "It is between :// and the next /."))
```

---

## http and https

The web's own protocol is HTTP, the Hypertext Transfer Protocol: how
a browser asks for a page and how the server answers. HTTPS is HTTP
made private and safe from tampering, the subject of a later lesson.
Most of the web today uses `https`.

```sources
(source "Zeitlos docs: web_app.md; Timeless Computing (docs/tc.md), Communication: Applications")
```

```check
(choice
  (ask "A URL starts with http://. What does that part tell web?")
  (right "Which protocol to use to fetch the page")
  (wrong "Which server to ask"
    "The server is named by the host name, after the two slashes. The part before :// is the protocol.")
  (wrong "That the page has pictures"
    "The scheme says nothing about what is on the page, only how to fetch it.")
  (hint "This part is called the scheme."))
```

---

## Opening a page

Open `web` from the dock. Its address field, at the top, already holds
`https://`, with the cursor after it: type a host name and press
**Enter**.

To type a new address later, press **F4** (or `g`, or Ctrl+L): the
address field takes the keys. **Enter** goes there; **Esc** puts back
the address that was there before. F4 is the prompt key in every app
([function keys](docs:keys)).

```check
(choice
  (ask "You are reading a page and want to go to another address. What do you press first?")
  (right "F4")
  (wrong "F1"
    "F1 shows web's help. F4 gives the address field the keys, so you can type the new URL.")
  (wrong "Enter"
    "Enter follows the selected link or, in the address field, goes to what you typed. First the field needs the keys.")
  (hint "It is the key for an app's prompt, the same in every app."))
```

---

## Links

A **link** is words on a page that lead to another page. Press **Tab**
to select the next link, and **Enter** to follow it, or click it.

To go back to the page before, press `b` or **Backspace**; `f` goes
forward again. The two arrow buttons beside the address do the same.

```try
(try (app "web")
  (task "Open web from the dock. Type a host name you know into the address field and press Enter. Press Tab to select a link and Enter to follow it, then press b to come back. Then come back here.")
  (check (choice
    (ask "After following a link, which key took you back?")
    (right "b (or Backspace)")
    (wrong "Esc" "Esc stops a page that is loading, or puts back the address while you edit it. Back is b, or Backspace.")
    (wrong "F12" "F12 is home: the page web started with, or a list of pages you kept. b goes back one page."))))
```

```check
(choice
  (ask "How do you select the next link on a page without the mouse?")
  (right "Tab")
  (wrong "The Down arrow"
    "The arrows scroll the page. Tab moves from link to link.")
  (wrong "F3"
    "F3 finds text in the page. Tab selects links.")
  (hint "The same key that moves from button to button in a dialog."))
```

---

## Reloading and stopping

**F5** (or `r`) fetches the page again. **F12** goes home: the page
`web` was started with or, if it was started with none, a list of
pages you have kept (the next lesson) ([web](docs:web_app)).

Pages take time here: a large page can take half a minute or more to
arrive and be laid out. While a page loads, the Go button becomes
**Stop**, and **Esc** stops it too.

```check
(choice
  (ask "A page is taking too long to load and you want to give up. What do you press?")
  (right "Esc")
  (wrong "F5"
    "F5 reloads: it starts fetching the page again, which will take as long. Esc stops.")
  (wrong "F12"
    "F12 goes home, but Esc is the key that stops a load.")
  (hint "The same key that cancels most things."))
```

---

```check
(text
  (ask "In the URL https://example.org/news.html, what is the path?")
  (accept "/news.html") (accept "news.html")
  (wrong-if "example.org" "That is the host name: the server. The path is what follows it.")
  (wrong-if "https" "That is the scheme. The path comes last.")
  (hint "Everything from the / after the host name."))
```

---

```check
(choice
  (ask "Which key reloads the page in web?")
  (right "F5")
  (wrong "F4" "F4 gives the address field the keys. F5 reloads.")
  (wrong "F12" "F12 goes home. F5 reloads.")
  (hint "In web, F5 is r's twin."))
```

So: a URL is a scheme, a host name and a path; F4 types a new one,
Tab and Enter follow links, b goes back, F5 reloads and Esc stops.
