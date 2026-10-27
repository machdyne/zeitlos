# Gopher

```lesson
(objective "Open a Gopher menu in web, tell its kinds of item apart, and read a Gopher URL.")
(minutes 15)
(keywords "Gopher" "small internet" "selector (Gopher)" "character (everyday word)")
```

This lesson is about Gopher, an older and simpler way to publish on the
internet, which `web` also reads.

The web grew into pages that are programs, far too heavy for a small
computer. Some people publish instead where the documents are mostly
text, sent as one request and one answer: the **small internet**.
Gopher is one of its protocols; the next lesson is about another.

**Gopher** is a protocol built around **menus**: a server offers a list
of items, each a text document, another menu, or a search. You move
from menu to menu until you find what you want to read.

```sources
(source "Zeitlos docs: gopher_gemini.md")
(source "Timeless Computing (docs/tc.md), Communication: Applications")
```

```check
(choice
  (ask "What is a Gopher server's main way of showing what it has?")
  (right "A menu of items, each leading to a document, a menu or a search")
  (wrong "A page laid out with pictures and columns"
    "Gopher has no page layout. It offers lists of items, and the documents are mostly plain text.")
  (wrong "A program that runs in the browser"
    "Nothing runs in a Gopher client but the client itself. The server sends menus and text.")
  (hint "It is built around lists you choose from."))
```

---

## A menu in web

Type a URL that starts with `gopher://` into the address field, as for
any page. `web` shows a menu as it was designed, in columns, and marks
each item with what it is:

```
      Welcome to an example server
[dir] Phlog
[txt] About this server
[ ? ] Search
[www] Project page
[bin] Disk image
```

`[dir]` is another menu, `[txt]` a text document, `[ ? ]` a search,
`[www]` a web page. Those are links: Tab and Enter, or a click, as on
any page. A file such as `[bin]` is shown for what it is, but is not a
link: `web` has nothing that could open it.

```check
(choice
  (ask "In a Gopher menu in web, an item is marked [dir]. What happens when you follow it?")
  (right "Another menu opens")
  (wrong "A text document opens"
    "Text documents are marked [txt]. [dir] is a directory: another menu.")
  (wrong "Nothing: it is not a link"
    "Menus, text, searches and web links are all links. Only files web cannot open, such as [bin], are not.")
  (hint "dir is short for directory: a list of items."))
```

---

## Searching

An item marked `[ ? ]` is a search. When you follow it, the address
field asks for the words to look for: type them and press **Enter**.
The server answers with a menu of what it found.

```check
(choice
  (ask "You follow a [ ? ] item. What does web want from you next?")
  (right "The words to search for, typed in the address field")
  (wrong "A password"
    "Gopher has no logins. A [ ? ] item is a search, so web asks what to search for.")
  (wrong "Nothing: the results appear at once"
    "The server cannot search for nothing. You type the words first, then press Enter.")
  (hint "[ ? ] marks a search."))
```

---

## A Gopher URL

In a Gopher URL, the first **character** of the path (a letter, digit
or sign) says what kind of
item it is: `1` a menu, `0` a text document, `7` a search. After it
comes the item's name on the server, its **selector**.

```
gopher://example.org/1/phlog       the menu /phlog
gopher://example.org/0/about.txt   the text document /about.txt
gopher://example.org/              the server's main menu
```

```check
(text
  (ask "In gopher://example.org/0/notes.txt, which character says that the item is a text document?")
  (accept "0")
  (wrong-if "1" "1 marks a menu. A text document is another number.")
  (wrong-if "7" "7 marks a search.")
  (hint "Look at the first character after the host name and its /."))
```

---

## Plain, and kept short

Gopher sends everything as it is: it has no encryption. Use it for
reading, not for anything private.

A Gopher answer has no length given in advance: it ends when the server
closes the connection. So `web` stops at 4 MB and shows what arrived,
with a note saying it was cut short.

```check
(choice
  (ask "Is a Gopher page protected on its way to you, like an https page?")
  (right "No: Gopher sends everything as it is")
  (wrong "Yes: web encrypts every page"
    "web encrypts only where the protocol itself does, as https does. Gopher has no encryption.")
  (hint "Gopher has nothing like TLS."))
```

---

```check
(choice
  (ask "In a Gopher menu, which item can web not open?")
  (right "[bin] Disk image")
  (wrong "[txt] About this server"
    "A text document is what Gopher is for, and web shows it.")
  (wrong "[www] Project page"
    "A web page web can certainly open.")
  (wrong "[dir] Phlog"
    "[dir] is a menu, and web shows menus.")
  (hint "Which of these is a file, not a menu or text?"))
```

So: Gopher is menus and text; `web` marks each item, follows menus,
text, searches and web links, and keeps no secrets on the way.
