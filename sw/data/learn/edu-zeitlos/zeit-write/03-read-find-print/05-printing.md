# Printing

```lesson
(objective "Know what Zeitlos can print today, set up a printer, and get writing onto paper.")
(minutes 20)
(keywords "network (of computers)" "printer address" "export (a document)")
```

Zeitlos prints through an app called `print`. It sends pages to a
printer on the **network**: the computers and printers joined by cable
or radio so that they can send each other data.

Today (2026), only `slide` prints (with Ctrl+P). `text` and
`read` cannot print yet: the parts are ready, but they are not joined
to `print`.

```check
(choice
  (ask "You press Ctrl+P in text to print a letter. What happens?")
  (right "Nothing: text cannot print yet")
  (wrong "The letter prints"
    "Printing from text is not there yet. Only slide prints today.")
  (wrong "print asks which printer to use"
    "text does not start print at all yet. Only slide does.")
  (hint "Which app prints today?"))
```

```sources
(source "Zeitlos docs: print (docs/print.md), Using it; Not yet")
```

---

## The printer's address

`print` needs the printer's **printer address**: where on the network
to find it. It looks like `192.168.1.50`, or written in full,
`ipp://192.168.1.50/ipp/print`. The printer shows it on its own panel,
in its network settings.

Most network printers made since about 2013 take pages this way with
nothing to install: they are called driverless, IPP Everywhere or
AirPrint.

```check
(choice
  (ask "Where can you find your printer's address?")
  (right "On the printer's own panel, in its network settings")
  (wrong "In the settings app"
    "settings has no row for the printer. The printer itself shows its address.")
  (wrong "It is always 192.168.1.50"
    "That is only an example. Each printer has its own address on its network.")
  (hint "Ask the printer."))
```

---

## Setting the printer

The address goes in the file `/sys/zeitlos.cfg`, on a line of its own:

```
apps.print.printer: 192.168.1.50
```

Double-click `zeitlos.cfg` in `files` (in the folder `/sys`) and it
opens in `text`. Add the line, save, then press **Reload file** in
`settings` so the change is used at once.

```check
(text
  (ask "Your printer's address is 10.0.0.7. What line goes in /sys/zeitlos.cfg?")
  (accept "apps.print.printer: 10.0.0.7")
  (accept "apps.print.printer:10.0.0.7")
  (accept "apps.print.printer = 10.0.0.7")
  (accept "apps.print.printer=10.0.0.7")
  (accept "apps.print.printer 10.0.0.7")
  (accept "apps.print.printer: ipp://10.0.0.7/ipp/print")
  (wrong-if "10.0.0.7" "The address alone is not enough: the line starts with the setting's name, apps.print.printer, then a colon.")
  (hint "The name, a colon, then the address.")
  (hint "apps.print.printer: then the address"))
```

```sources
(source "Zeitlos docs: print (docs/print.md), Setting up; /sys/zeitlos.cfg (sw/data/zeitlos.cfg)")
```

---

## The paper

Without a setting, `print` uses US Letter paper. For A4, add:

```
apps.print.paper: a4
```

```check
(choice
  (ask "You print on A4 paper and have set only apps.print.printer. What paper does print ask for?")
  (right "Letter")
  (wrong "A4"
    "print does not know what is in the tray. Without apps.print.paper, it uses Letter.")
  (hint "What is the setting when there is no line?"))
```

---

## Trying the printer

Run `print` by itself to test the printer: in `files`, go into `/apps`
and open `print`. It asks the printer what it is and what it takes,
and shows the answer until you press OK. If that works, printing will.

If no address is set, it says "No printer: set apps.print.printer
(docs/print.md)".

```check
(choice
  (ask "print says: No printer: set apps.print.printer. You did add the line. What is the likely reason?")
  (right "The file was saved, but Reload file was not pressed in settings")
  (wrong "The printer is switched off"
    "Then print would have an address and fail to reach it. No printer means it has no address at all.")
  (hint "Is the new line in use yet?"))
```

---

## Writing onto paper today

Until `text` prints, **export** your document: save a copy in another
system's format. In `text`, press Ctrl+Shift+S and give a name ending
in `.docx`, `.odt` or `.rtf`. The copy is written for you; your
document keeps its own name. A `.md` keeps its headings, lists, bold
and italic. Then print the copy from a computer that has a word
processor.

```check
(choice
  (ask "You export plan.md as plan.docx with Ctrl+Shift+S. Which name does the text window have afterwards?")
  (right "plan.md")
  (wrong "plan.docx"
    "An export is a copy. Unlike a usual Save As, the window keeps its own name.")
  (hint "Export makes a copy."))
```

```sources
(source "Zeitlos docs: text (docs/text_editor.md), Other systems' formats; convert (docs/convert.md)")
```

---

```try
(try (app "files")
  (task "In files, go into /apps and open print (select it and press Enter). Read what its window says, press OK, and come back here.")
  (check (choice
    (ask "If it said No printer, what is missing?")
    (right "The line apps.print.printer, in use")
    (wrong "Software to install for the printer"
      "print needs nothing installed: the printer says what it takes when asked. It needs the address.")
    (wrong "Paper in the printer"
      "print has not reached any printer yet. It needs the address first."))))
```

So: only `slide` prints today; set the printer's address in
`/sys/zeitlos.cfg`, test it with `print`, and export from `text` to
put writing on paper.
