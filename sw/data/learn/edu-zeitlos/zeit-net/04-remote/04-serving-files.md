# Serving files

```lesson
(objective "Serve a folder of files as web pages from Zeitlos, and work out the URL of each file.")
(minutes 20)
(keywords "web server" "index.html")
```

This lesson is about putting your own pages and files on the network.

A **web server** answers HTTP requests with files. netserve has one.
Switch it on with a line in `/sys/zeitlos.cfg`: a port, then the folder
to serve.

```
apps.netserve.http: 80 /data/netserve/www
```

Then restart the computer. Every file in `/data/netserve/www`, and in
the folders inside it, can be fetched by any browser on your subnet,
with a URL that starts with `http://` and your computer's address.

```sources
(source "Zeitlos docs: netserve.md, The web server; Testing on a board")
```

```check
(choice
  (ask "Which folder does the line apps.netserve.http: 80 /data/netserve/www serve?")
  (right "/data/netserve/www, and the folders inside it")
  (wrong "Your whole card"
    "Only the folder named, and what is inside it. Nothing outside it can be fetched, however the URL is written.")
  (wrong "/home"
    "/home is not mentioned in the line. The folder is the second thing after the port.")
  (hint "The line has two things: a port, then a folder."))
```

---

## From file to URL

The path in the URL is the file's path inside the served folder. If
your computer's address is `192.168.1.50`:

| File | URL |
|---|---|
| /data/netserve/www/notes.txt | http://192.168.1.50/notes.txt |
| /data/netserve/www/pics/cat.png | http://192.168.1.50/pics/cat.png |

```example
(example
  (problem "Your computer is 192.168.1.50. What is the URL of /data/netserve/www/docs/plan.html?")
  (step "Take away the served folder, /data/netserve/www, from the front: /docs/plan.html is left.")
  (step "Put http:// and the address in front.")
  (answer "http://192.168.1.50/docs/plan.html"))
```

```check
(text
  (ask "Your computer is 192.168.1.50. What is the URL of /data/netserve/www/menu.html?")
  (accept "http://192.168.1.50/menu.html") (accept "192.168.1.50/menu.html")
  (wrong-if "http://192.168.1.50/data/netserve/www/menu.html" "The served folder itself is not part of the URL: the URL's path starts inside it.")
  (wrong-if "https://192.168.1.50/menu.html" "netserve's web server speaks plain http, not https.")
  (hint "Take away /data/netserve/www, then put http:// and the address in front."))
```

---

## The front page

A URL that names a folder, such as `http://192.168.1.50/`, gets the
file **index.html** in that folder (or, failing that, `index.htm`).
There is no list of a folder's files: a folder without an index page
cannot be looked into.

So the first file to make is `/data/netserve/www/index.html`: your
front page. Write it in `text`.

```check
(choice
  (ask "Someone opens http://192.168.1.50/ in a browser. Which file do they get?")
  (right "/data/netserve/www/index.html")
  (wrong "A list of the files in /data/netserve/www"
    "netserve shows no lists of files. A folder's URL gets its index.html.")
  (wrong "/data/netserve/www/home.html"
    "The name netserve looks for is index.html.")
  (hint "Every folder's front page has the same name."))
```

---

## What it will and will not do

- It only **reads**: nobody can change or add a file through it.
- It needs no password. Everything in the folder can be fetched by
  anyone who may connect, so put nothing private there.
- Each request is logged on the console: who asked, for what, and the
  answer.
- It speaks plain http: what it sends is not encrypted.
- As every netserve service, it is for your subnet only unless you say
  `any`, and only on a board with Ethernet.

```check
(choice
  (ask "You put your tax letters in /data/netserve/www/private, a folder without an index page. Are they safe there?")
  (right "No: anyone who may connect and guesses a file's name can fetch it")
  (wrong "Yes: a folder without an index page cannot be opened"
    "The folder cannot be listed, but each file in it can still be fetched by its URL. No password protects it.")
  (wrong "Yes: netserve asks for the password"
    "The web server asks for none. Everything in the served folder is public to those who may connect.")
  (hint "Does the web server ask for anything before it sends a file?"))
```

---

```check
(choice
  (ask "Which scheme does netserve's web server answer?")
  (right "http")
  (wrong "https" "netserve's web server does not encrypt: it answers plain http.")
  (wrong "gemini" "netserve serves files over http, not Gemini."))
```

---

```check
(text
  (ask "What is the name of the file a folder's URL gets?")
  (accept "index.html") (accept "index.htm")
  (wrong-if "index" "Write the whole file name, with its ending.")
  (hint "index, then a dot, then the ending of a web page."))
```

So: one line serves a folder; a file's URL is `http://`, your address
and its path in the folder; `index.html` is the front page, and
everything there is public.
