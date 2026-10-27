# Pictures and sound

```lesson
(objective "Work out how much memory a picture needs, and say how sound is stored as samples.")
(minutes 20)
(keywords "pixel" "bits per pixel" "sample (of sound)" "sample rate" "sample size")
```

A picture on a screen is a grid of **pixels**, tiny dots. Each pixel is
a number that gives its colour. The **bits per pixel** decide how many
colours there can be:

| Bits per pixel | Colours |
|---|---|
| 1 | 2 (black and white) |
| 4 | 16 |
| 8 | 256 |
| 24 | about 16.7 million |

24 bits are 8 for each of red, green and blue.

```sources
(source "Timeless Computing (docs/tc.md), Information: Pictures and Sound")
```

```check
(number
  (ask "How many colours can a pixel of 2 bits show?")
  (answer 4) (from "2^2")
  (wrong-if 2 "That is 1 bit. Each extra bit doubles the number of colours.")
  (hint "n bits make 2 to the power n patterns."))
```

---

## How much memory

A picture needs width x height x bits per pixel bits. A 640 by 480
screen at 1 bit per pixel needs 307,200 bits, which is 38,400 bytes.
The same screen in 24-bit colour needs 921,600 bytes, 24 times as
much. On a small computer that difference decides what is possible,
which is one reason simple machines use few colours.

```example
(example
  (problem "How many bytes does a 320 by 240 picture need at 8 bits per pixel?")
  (step "320 x 240 = 76,800 pixels.")
  (step "8 bits is one byte, so each pixel takes one byte.")
  (answer "76,800 bytes."))
```

```check
(number
  (ask "How many bytes does a 320 by 240 picture need at 1 bit per pixel?")
  (answer 9600) (from "320 * 240 / 8")
  (wrong-if 76800 "That is the number of pixels, or bits. There are 8 bits in a byte.")
  (wrong-if 614400 "That multiplies by 8. One bit per pixel means 8 pixels share each byte, so divide by 8.")
  (hint "320 x 240 bits, divided by 8 bits in a byte."))
```

---

## Sound

Sound is a wave of air pressure. It is stored by measuring the wave
many times a second; each measurement is a **sample**. Two numbers
describe digital sound:

- the **sample rate**: how many samples a second;
- the **sample size**: how many bits each sample has.

Move the sliders. The smooth line is the sound; the steps are what is
stored.

```figure
(define r (slider 'r 4 32 12 "Samples" 1))
(define b (slider 'b 1 4 3 "Bits" 1))
(define lv (- (expt 2 b) 1))
(define (wave x) (sin (* 2 pi x)))
(path (let loop ((k 48) (acc '()))
        (if (< k 0) acc
            (loop (- k 1) (cons (+ 20 (* 280 (/ k 48))) (cons (- 110 (* 80 (wave (/ k 48)))) acc)))))
      'grey 1)
(for-range 0 r
  (lambda (k)
    (let* ((q (/ (round (* lv (/ (+ 1 (wave (/ k r))) 2))) lv))
           (y (- 190 (* 160 q))) (x (+ 20 (* 280 (/ k r)))))
      (line x y (+ 20 (* 280 (/ (+ k 1) r))) y 'ink 2)
      (dot x y 'ink))))
(caption (string-append (fmt r 0) " samples, " (fmt (+ lv 1) 0) " levels each"))
```

```check
(choice
  (ask "What does a higher sample rate let a recording capture?")
  (right "Higher pitches")
  (wrong "Quieter detail"
    "Quieter detail needs more bits per sample, the sample size. A higher rate means more measurements a second, which can follow faster waves.")
  (wrong "Louder sounds"
    "Loudness is how big the wave is, and the sample size sets the range of values. The rate sets how fast a wave it can follow.")
  (hint "More samples a second can follow a wave that goes up and down faster."))
```

---

## A compact disc

Compact discs use 44,100 samples a second at 16 bits a sample, for
each of two channels (left and right).

```check
(number
  (ask "One channel of CD sound: 44,100 samples a second, 2 bytes each. How many bytes a second?")
  (answer 88200) (from "44100 * 2")
  (wrong-if 44100 "That is the number of samples. Each 16-bit sample takes 2 bytes.")
  (wrong-if 705600 "That is in bits: 44,100 x 16. In bytes, each sample is 2.")
  (hint "16 bits are 2 bytes. Multiply by the samples a second."))
```

---

```check
(number
  (ask "How many colours can a pixel of 8 bits show?")
  (answer 256) (from "2^8")
  (wrong-if 8 "8 is the number of bits. Each bit doubles the colours: 2 to the power 8.")
  (hint "A byte holds 256 patterns."))
```

So: a picture is a grid of numbered pixels, sound is a row of samples,
and both cost memory in proportion to their detail.
