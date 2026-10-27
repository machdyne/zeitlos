# USB, keyboard and mouse

```lesson
(objective "Follow a key press from a USB keyboard to an app, and say why the keyboard keeps working if the usb driver stops.")
(minutes 15)
(keywords "HID registers")
```

You will learn how a key you press reaches the app you are typing in.

Boards differ here. Most Machdyne boards, the Mozart and Sergei boards
and Lakritz among them, have a full **USB host** controller in region
C, driven by the `usb` driver: it sets up hubs, keyboards, mice, USB
sticks, USB Ethernet and USB serial adapters. A few boards, such as the
Obst and the ULX3S, have a simpler controller built only for a keyboard
and a mouse; there `usb` is not started at all.

```sources
(source "Zeitlos source: rtl/boards.vh, USB_HOST and USB_HID in each board block")
(source "Zeitlos docs: usb_driver.md, Booting (only with a USB host in the bitstream)")
```

```check
(choice
  (ask "On an Obst, which program sets up the keyboard?")
  (right "None: its keyboard and mouse controller is hardware built for just that")
  (wrong "The usb driver" "On boards without a USB host the usb driver is not started. The keyboard controller there needs no driver to set it up.")
  (wrong "The window manager" "wm receives keys, but it does not set up USB devices."))
```

---

## From the key to the app

1. When a keyboard or mouse is plugged in, `usb` sets it up.
2. From then on the USB controller asks the keyboard for reports by
   itself, and puts each one in the kernel's **HID registers** (HID is
   the USB class for keyboards and mice). No program is in this path.
3. Each report raises interrupt line 5 or 6. The kernel reads the key
   and sends it to `wm` in a message.
4. `wm` sends it on to the window that has the focus.

```sources
(source "Zeitlos docs: usb_driver.md, What moved, and what did not; kernel.md, The scheduler (a message sent from an interrupt handler)")
(source "Zeitlos source: sw/os/kernel.h, Z_IRQ_HID and Z_IRQ_HID1")
```

```check
(choice
  (ask "A key is pressed. Which program sends it to the app with the focus?")
  (right "wm, the window manager")
  (wrong "usb" "usb sets the keyboard up, but once it is set up the controller and the kernel deliver keys to wm without it.")
  (wrong "The kernel, straight to the app" "The kernel sends keys to wm, which knows which window has the focus."))
```

---

## When usb stops

Because step 2 needs no program, a keyboard or mouse that is already
set up **keeps working if `usb` crashes**. What stops until `usb` is
back is setting up new devices, USB sticks, USB Ethernet and serial.

`usb` itself is woken by the USB host's interrupt, line 9: the kernel
quietens the controller and wakes `usb`, which does the work as a
process. No USB work is done in the kernel's tick any more.

```sources
(source "Zeitlos docs: usb_driver.md, What moved, and what did not (keyboards keep working if usb dies); kernel.md, What a tick costs")
```

```check
(choice
  (ask "usb crashes while you are typing. What happens to your keyboard?")
  (right "It keeps working: its reports reach the kernel without usb")
  (wrong "It stops until usb is restarted"
    "Once set up, the keyboard is polled by the controller itself. Only new devices wait for usb.")
  (wrong "The machine stops"
    "A crashed driver ends alone, and the kernel starts it again a second later."))
```

---

```check
(number
  (ask "Which interrupt line wakes the usb driver?")
  (answer 9)
  (wrong-if 5 "Line 5 is a keyboard and mouse port's report. The host controller has line 9.")
  (hint "The USB host controller's own line."))
```

So: a key goes from the USB controller to the kernel's HID registers to `wm` to the app, with no driver in the path once the keyboard is set up.
