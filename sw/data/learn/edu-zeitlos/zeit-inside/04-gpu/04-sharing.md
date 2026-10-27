# Sharing the screen

```lesson
(objective "Explain how many apps draw into one framebuffer without drawing over each other's windows.")
(minutes 15)
(keywords "window manager" "visible region" "repair (of the screen)")
```

You will learn how windows share the one framebuffer.

There is one framebuffer and no copy of each window anywhere. Every app
draws its own window straight into VRAM with the blitter and the
rasterizer. What keeps it inside its window is the **window manager**,
`wm`: the program that owns the screen, draws the windows' frames and
titlebars, and tells each app where it may draw.

For each window, `wm` works out its **visible region**: its rectangle,
minus every window in front of it. That is a list of rectangles, not
one: a window with another in its middle is visible as a ring. The app's
drawing library then confines every fill, copy and line to that list.

```sources
(source "Zeitlos docs: window_manager.md, Overview; Visible regions")
```

```check
(choice
  (ask "A clock app keeps drawing its hands while a text editor's window lies over part of it. Why do the hands not appear over the editor?")
  (right "The clock may draw only in its visible region, which leaves out the part under the editor")
  (wrong "wm draws the editor again after every tick of the clock"
    "That would flash and waste time. The clock's drawing never reaches the editor's pixels at all.")
  (wrong "Each window has its own framebuffer, combined when shown"
    "Zeitlos has one framebuffer and no copies of windows. Confining each app's drawing is what keeps them apart.")
  (hint "What does wm send each app?"))
```

---

## Clip registers are shared

The blitter and the rasterizer each have one clip rectangle, in hardware,
with no notion of which process wrote it. A context switch can come
between a program setting its clip and drawing with it. So the kernel
saves the GPU's clip registers when it switches a process out and puts
them back when it switches it in, exactly as it does the CPU's registers.
That costs 40 bytes for each process slot.

```sources
(source "Zeitlos docs: kernel.md, The scissor is part of the context")
```

```check
(choice
  (ask "Why must the kernel save the GPU's clip rectangle at a context switch?")
  (right "Another process could change it between this process setting it and drawing")
  (wrong "Because the GPU forgets its registers when the CPU switches"
    "The hardware keeps whatever was written last. That is the problem: it may be another process's rectangle.")
  (wrong "Because only the kernel may write the clip registers"
    "Apps write them directly. The kernel cannot stop that, so it saves and restores them instead."))
```

---

## Repairs, not redraws

When something changes, `wm` does a **repair**: it redraws only the
rectangle that changed, and asks only the apps whose windows overlap it
to redraw their part. Windows elsewhere are not touched, so they do not
flash.

Dragging a window is a special case. While the button is held, `wm`
freezes every window's drawing and moves only an XOR outline; the
window itself stays where it was until you let go. Then two rectangles
are repaired: where it was, and where it went.

```sources
(source "Zeitlos docs: window_manager.md, Redraw strategy; Freezing the desktop for a drag")
```

```try
(try (app "clock")
  (task "Open clock from the dock (it is in the Desk group). Drag its window by its titlebar slowly across the screen, and watch both the window and its hands while you drag. Let go, then come back here.")
  (check (choice (ask "What moved while you held the button down?")
    (right "Only an outline: the window stayed where it was until I let go")
    (wrong "The whole window, with its hands turning as it went"
      "wm moves only an XOR outline while you drag, and freezes the windows' drawing so the outline can be taken away cleanly. Try it again slowly.")
    (wrong "Nothing at all until I let go"
      "Look again: an outline of the window follows the pointer. It is drawn with XOR, so it shows on any background."))))
```

---

```check
(choice
  (ask "A window is closed. Which part of the screen does wm repair?")
  (right "Only the rectangle the closed window covered")
  (wrong "The whole screen"
    "A full redraw would make every window flash. wm repairs just the area that changed.")
  (wrong "Nothing: the apps behind redraw themselves without being asked"
    "They redraw when wm tells them to, and it tells only those whose windows overlap the uncovered rectangle."))
```

So: one framebuffer is shared by giving each app a visible region to draw in, saving the GPU's clip with each process, and repairing only what changed.
