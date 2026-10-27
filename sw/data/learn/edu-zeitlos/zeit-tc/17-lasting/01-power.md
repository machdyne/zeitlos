# Power and conditions

```lesson
(objective "Work out how much energy a computer needs each day, and say what makes electronics age.")
(minutes 20)
(keywords "watt-hour" "Wh")
```

You will plan power for a small computer, and see what makes
electronics wear out.

Electricity, spare parts, updates and the Internet are usually there,
but not guaranteed. Planning for their loss is a design exercise, not a
prediction: what helps a computer through hard times also makes it
cheaper, simpler and easier to own in ordinary ones.

A supply is planned from how much energy the equipment needs. Energy
is power multiplied by time, measured in **watt-hours**, **Wh**: one
watt for one hour. A computer drawing 3 W for 8 hours a day needs
3 times 8 = 24 Wh a day.

```sources
(source "Timeless Computing (docs/tc.md), Computing That Lasts: Power Supply")
```

```check
(number
  (ask "A computer draws 4 W and is used 6 hours a day. How many watt-hours does it need each day?")
  (answer 24) (unit "Wh") (from "4 * 6")
  (wrong-if 10 "That adds the watts and the hours. Energy is power multiplied by time.")
  (wrong-if 1.5 "That divides the hours by the watts. Energy is power multiplied by time: 4 times 6.")
  (hint "Watts times hours."))
```

---

## A power bank

A common USB power bank stores about 37 to 74 Wh, of which perhaps two
thirds reaches the computer at 5 V after conversion losses. Move the
sliders: how many days does a 37 Wh bank last?

```figure
(define w (slider 'w 1 10 3 "Power (W)" 0.5))
(define h (slider 'h 1 24 8 "Hours a day" 1))
(define need (* w h))
(define give (/ 74 3))
(define days (/ give need))
(text 20 40 "Needed each day" 'small)
(rect 20 48 (clamp (* 2.8 need) 1 280) 24 'warm)
(frame 20 48 280 24 'ink 1)
(text 20 100 "The bank delivers" 'small)
(rect 20 108 (* 2.8 give) 24 'cool)
(frame 20 108 280 24 'ink 1)
(text 160 170 (string-append (fmt need 1) " Wh a day; the bank gives " (fmt give 1) " Wh") 'centre 'small)
(text 160 200 (string-append "It lasts " (fmt days 1) " days") 'centre 'body 'bold)
(caption "A whole bar is 100 Wh.")
```

```check
(number
  (ask "A bank delivers 24 Wh. A computer draws 2 W for 6 hours a day. How many days does the bank last?")
  (answer 2) (unit "days") (from "24 / (2 * 6)")
  (wrong-if 12 "12 Wh is what the computer needs each day. Divide the bank's 24 Wh by it.")
  (wrong-if 4 "That divides by the hours alone. Each day needs 2 W times 6 hours: 12 Wh.")
  (hint "First the energy a day: watts times hours.")
  (hint "Then divide what the bank delivers by that."))
```

```sources
(source "Timeless Computing (docs/tc.md), Computing That Lasts: Power Supply")
```

---

## The sun, and saving energy

A small solar panel can put the energy back. A 20 W panel might give
about 50 to 100 Wh on a sunny day, depending on the season, the place
and the weather, and far less on a cloudy one. A practical system has a
generous margin, and batteries for several cloudy days.

Energy lasts longer when:

- **the display is small**, since it often uses the most power;
- **unused equipment is off**, such as a module that sleeps;
- **the clock is slower**, since chips use power mainly when their
  transistors switch;
- **conversions are few**: each change of voltage loses some energy.

```check
(choice
  (ask "A small computer and its monitor run from a battery. What is often the biggest single saving?")
  (right "A smaller display, since the display often uses the most power")
  (wrong "A faster processor, to finish sooner"
    "A faster clock uses more power, not less. The display is often the biggest user.")
  (wrong "More voltage converters"
    "Each conversion loses energy. Fewer conversions save energy.")
  (hint "Which part of the setup often draws more than the computer?"))
```

```sources
(source "Timeless Computing (docs/tc.md), Computing That Lasts: Power Supply")
```

---

## Heat

Electronics age at a rate set by their conditions, and **heat** is the
greatest cause. A rough rule for many parts is that each 10 °C more
halves their life. Move the temperature.

```figure
(define t (slider 't 20 70 30 "°C" 5))
(define (life x) (expt 2 (/ (- 20 x) 10)))
(graph 50 16 250 160 20 70 0 1)
(graph-axes 10 0.5)
(plot life 'ink)
(plot-point t (life t) 'warm)
(text 175 206 "temperature (°C)" 'centre 'tiny)
(text 175 226 (string-append "life: " (fmt (life t) 2) " of its life at 20 °C") 'centre 'small)
```

```check
(number
  (ask "A part lasts 16 years at 25 °C. By the rough rule, about how many years at 45 °C?")
  (answer 4) (unit "years") (from "16 / 2^2")
  (wrong-if 8 "That is one halving, for 10 °C. 45 °C is 20 °C warmer: two halvings.")
  (wrong-if 12 "Life halves for each 10 °C; it does not lose a fixed number of years.")
  (hint "45 is 25 plus two lots of 10."))
```

```sources
(source "Timeless Computing (docs/tc.md), Computing That Lasts: The Environment")
```

---

## Damp, dust and static

**Moisture** corrodes contacts and boards: keep equipment dry, and let
equipment brought in from the cold warm up before switching it on, so
that condensation can dry. **Dust** blocks cooling and can conduct when
damp. **Static electricity** destroys chips. An enclosure protects
against all of these.

```check
(choice
  (ask "A computer is carried in from a freezing shed into a warm room. What should you do?")
  (right "Let it warm up before switching it on")
  (wrong "Switch it on at once, so its own heat dries it"
    "Water condenses on cold equipment in warm air. Powered while wet, it can short or corrode. Wait for it to warm.")
  (wrong "Put it by a heater to dry it quickly"
    "Strong heat ages parts. Let it come to room temperature by itself.")
  (hint "Cold things in warm air collect water."))
```

```sources
(source "Timeless Computing (docs/tc.md), Computing That Lasts: The Environment")
```

So: energy is power times time, and equipment lasts longest kept cool,
dry, clean and enclosed.
