# Reading, finding and keeping pages

```lesson
(objective "Scroll and search a page in web, load its pictures, bookmark it and save it.")
(minutes 20)
(keywords "bookmark (web)" "source (of a page)" "JavaScript")
```

This lesson is about reading a page in `web` and keeping what you find.

`web` shows a page as one column of text, with headings, lists and
tables. Scroll with the arrows, **Space** or Page Down (Page Up goes
back), the mouse wheel, or **Home** and **End** for the top and the end.

**Pictures** are drawn as boxes at first. Click a box to load its
picture in place. One picture is loaded at a time, and **Esc** stops
one that is loading.

```check
(choice
  (ask "A page shows an empty box where a picture should be. What do you do to see the picture?")
  (right "Click the box")
  (wrong "Press F5"
    "F5 reloads the whole page, and the picture is still a box afterwards. Pictures load when you click their box.")
  (wrong "Nothing: web cannot show pictures"
    "It can: BMP, GIF, JPEG, PNG and SVG pictures are drawn, in black and white, once you click the box.")
  (hint "The box itself is the button."))
```

---

## Finding words

Press **F3** (or Ctrl+F) and type the words to find. The page moves to
the first match and draws a box round it. **F3** again finds the next
one; after the last, it goes round to the top and says so. Capital
letters do not matter. F3 is the find key in every app.

```check
(choice
  (ask "You found the word 'clock' once on a long page. How do you find where it comes next?")
  (right "Press F3 again")
  (wrong "Press F5"
    "F5 reloads the page and loses your place. F3 again finds the next match.")
  (wrong "Type the word again with Ctrl+F"
    "That works, but you need not: F3 on its own goes to the next match of what you asked for.")
  (hint "The key that found it the first time."))
```

---

## Bookmarks

A **bookmark** is a page's URL kept in a list, so you can come back to
it. **Ctrl+D** bookmarks the page you are reading; on a page that is
already bookmarked, it asks whether to remove it. **Ctrl+B** shows the
list as a page of links.

**F12** goes home. If `web` was not started with a page, home is the
bookmarks. They are kept in a plain text file you can edit,
`/data/web/bookmarks.txt`, one URL a line.

```check
(choice
  (ask "You want to keep the page you are reading so you can find it next week. What do you press?")
  (right "Ctrl+D")
  (wrong "Ctrl+B"
    "Ctrl+B shows the bookmarks you already have. Ctrl+D adds this page to them.")
  (wrong "Ctrl+S"
    "Ctrl+S saves the page into a file of yours. To come back to the page on the server, bookmark it with Ctrl+D.")
  (hint "D, as many browsers have it."))
```

---

## Saving

A page arrives as text with marks in it that say what is a heading, a
link, a list: its **source**. **Ctrl+S** saves the source to a file:
a dialog asks where, starting in `/home`, and suggests a name.

To save what a link points to without opening it (a file to
download, say), select the link with Tab and press **Shift+Enter**.

```check
(choice
  (ask "What does Ctrl+S save?")
  (right "The page's source, as the server sent it")
  (wrong "A picture of the screen"
    "It saves the page itself, the text and its marks, not a picture of the screen.")
  (wrong "A bookmark"
    "Bookmarks are Ctrl+D. Ctrl+S makes a file you can open later without the network.")
  (hint "The text with its marks, before web lays it out."))
```

---

## What web does not do

Many sites today are programs as much as pages: they run
**JavaScript**, a programming language that the browser runs to build
the page. `web` runs none. A page that needs JavaScript to show its
text shows little or nothing here; a page that is a document (an
encyclopedia, a manual, a plain blog) reads well.

This is a choice, not a gap to be filled: a JavaScript engine alone would
be about as large as the whole of Zeitlos ([web](docs:web_app)).

```check
(choice
  (ask "A site shows only a heading and an empty page in web, but looks full on another computer. What is the likely reason?")
  (right "The site builds its page with JavaScript, which web does not run")
  (wrong "The page is too large for web"
    "web reads very large pages: it never holds the whole page at once. An empty page usually means it was meant to be built by a program.")
  (wrong "Your network is too slow"
    "A slow network makes a page take longer, but it still arrives in the end. Missing text is the page's design.")
  (hint "What does the other computer's browser run that web does not?"))
```

---

```check
(choice
  (ask "Where does F12 take you in web, if web was opened from the dock with no page?")
  (right "Your bookmarks")
  (wrong "The page before"
    "That is Back, b. F12 is home: the page web was started with, or, with none, the bookmarks.")
  (wrong "Nowhere: F12 does nothing in web"
    "F12 is home in web, as in term and files.")
  (hint "Home is the page web started with, if there was one."))
```

---

```check
(text
  (ask "Which key finds words in a page, in web and in other apps?")
  (accept "F3")
  (wrong-if "F4" "F4 is the prompt: in web, the address field. Finding is the key before it.")
  (hint "Ctrl+F does the same in web."))
```

So: click a box for its picture, F3 finds, Ctrl+D bookmarks, F12 goes
home, and Ctrl+S saves the page's source.
