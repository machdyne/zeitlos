# Saving a painting

```lesson
(objective "Save, open and start draw's pictures, stop a slow load or save, and use a picture in slide.")
(minutes 15)
(keywords "ZBM (draw's files)")
```

draw's title bar has three icons: **new**, **open** and **save**. The
first time you save a picture, draw asks for a folder and a name.

draw's pictures are **ZBM** files: a short header, then the page's
pixels. Give the name the ending `.zbm`, as in `boat.zbm`; then
double-clicking it in `files` opens it in draw.

```sources
(source "draw (docs/draw_app.md), Files")
```

```check
(text
  (ask "You save a painting of a boat. What is a good file name for it?")
  (accept "boat.zbm")
  (wrong-if "boat" "Without the ending, files does not know the picture is draw's. Add .zbm.")
  (wrong-if "boat.zsl" ".zsl is a slide deck. draw's pictures end in .zbm.")
  (hint "The name, a dot, and draw's ending."))
```

---

## Changes not saved

If you press new or open, or close the window, with changes not saved,
draw asks whether to save them first:

- **Yes** saves, then goes on;
- **No** goes on without saving, and the changes are lost;
- **Cancel** does nothing, and you keep painting.

```check
(choice
  (ask "You press new by mistake and draw asks: Save changes before continuing? You want to keep painting the picture you have. What do you choose?")
  (right "Cancel")
  (wrong "No" "No goes on to a new page and throws your changes away.")
  (wrong "Yes" "Yes saves the picture, but then goes on to a new, empty page.")
  (hint "Which answer does nothing?"))
```

---

## Loading and saving never stop the app

A file is read or written a part at a time, and the title shows how far
it is: `Loading NAME 45%  (Esc stops)`. Meanwhile you can still scroll
and choose tools.

**Esc** stops it, and nothing is lost:

- a load stopped: the drawing on screen is the one there before;
- a save stopped: the old file is as it was.

That is because a save writes a new file, `NAME.part`, and puts it in
place of the old one only when every byte is written.

```check
(choice
  (ask "You stop a save of boat.zbm with Esc halfway. What is in boat.zbm now?")
  (right "The picture as it was last saved")
  (wrong "Half of the new picture" "The new picture is written to boat.zbm.part first. Stopped, that is deleted, and boat.zbm is not touched.")
  (wrong "Nothing: the file is gone" "The old file is removed only once the new one is complete.")
  (hint "Where does draw write a save first?"))
```

---

## Colour pictures go to pix

A ZBM can also be a colour picture, made in `pix` (the next unit).
draw paints only in black and white, so it does not open one; it says
"That is a colour picture: open it in pix".

draw's pictures can go into slide: on a card, the image tool **i** asks
for a picture file, and a `.zbm` is one.

```check
(choice
  (ask "You open a colour .zbm in draw, and draw refuses. Which app opens it?")
  (right "pix")
  (wrong "slide" "slide can place a picture on a card, but to edit a colour picture you need pix.")
  (wrong "draw, after F1" "F1 is help. draw paints only in black and white.")
  (hint "draw's message names the app."))
```

---

```try
(try (app "draw")
  (task "Open draw from the dock and paint something. Click the save icon in the title bar and save it as test.zbm in your folder. Then come back.")
  (check (choice
    (ask "What does the window's title show now?")
    (right "The file's name, test.zbm")
    (wrong "untitled" "A picture that has never been saved is untitled. After saving, the title shows its name.")
    (wrong "Saving 100%" "The title shows Saving only while the save is going on."))))
```

So: draw's pictures are `.zbm` files; Esc stops a load or a save
without losing anything; and colour pictures are pix's.
