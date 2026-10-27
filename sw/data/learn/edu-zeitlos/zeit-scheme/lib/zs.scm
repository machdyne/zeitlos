; Programming in Scheme: what the course's figures draw.
; Names start with zs- so they cannot clash with the library's.

; the string s, k times over
(define (zs-repeat s k)
  (let loop ((i 0) (acc ""))
    (if (>= i k) acc (loop (+ i 1) (string-append acc s)))))

; the steps of (factorial n) worked out by hand, one string a step,
; as Timeless Computing (Scheme: Recursion) writes them
(define (zs-fact-lines n)
  (let loop ((k 0) (pre "") (acc '()))
    (if (> k n)
        (reverse
          (cons (number->string (zs-fact n))
                (cons (string-append pre "1" (zs-repeat ")" n)) acc)))
        (loop (+ k 1)
              (if (< k n) (string-append pre "(* " (number->string (- n k)) " ") pre)
              (cons (string-append pre "(factorial " (number->string (- n k)) ")"
                                   (zs-repeat ")" k))
                    acc)))))

(define (zs-fact n)
  (let loop ((i n) (acc 1))
    (if (< i 1) acc (loop (- i 1) (* acc i)))))

; strings down the figure from y, gap units apart, in a fixed face
(define (zs-lines x y gap lines)
  (let loop ((l lines) (y y))
    (if (pair? l)
        (begin (text x y (car l) 'small) (loop (cdr l) (+ y gap))))))

; a window as the desktop draws it: a frame and a title bar, the title
; in it. Returns nothing; its drawing area starts 13 units under y.
(define (zs-win x y w h title)
  (rect x y w 13 'ink)
  (text (+ x (/ w 2)) (+ y 10) title 'paper 'centre 'small)
  (frame x y w h 'ink 1.5))

; a chain of pairs, as Timeless Computing (Scheme: Lists) draws one:
; each pair a box with the item on the left and an arrow to the next
; on the right; the last pair's right holds (). From (x, y), each pair
; w units wide and step apart. A pair numbered from (`from`) on is
; drawn heavy: the part of the list that (cdr ...) still reaches.
(define (zs-pairs x y w step items from)
  (let loop ((l items) (i 0))
    (if (pair? l)
        (let ((bx (+ x (* i step))) (half (* w 0.62)))
          (if (>= i from) (frame bx y w 22 'ink 2.5) (frame bx y w 22 'grey 1))
          (line (+ bx half) y (+ bx half) (+ y 22) 'ink 1)
          (text (+ bx (/ half 2)) (+ y 15) (car l) 'centre 'small)
          (if (pair? (cdr l))
              (arrow (+ bx half 6) (+ y 11) (+ bx step) (+ y 11) 'ink 1)
              (text (+ bx half (/ (- w half) 2)) (+ y 15) "()" 'centre 'small))
          (loop (cdr l) (+ i 1))))))

; a window's drawing area in a figure: area-w x area-h pixels drawn s
; units a pixel, its corner at (zs-ax, zs-ay)
(define zs-ax 22)
(define zs-ay 44)
(define zs-s 1.4)
(define (zs-px x) (+ zs-ax (* zs-s x)))
(define (zs-py y) (+ zs-ay (* zs-s y)))
; the window around a drawing area of w x h pixels
(define (zs-area-win w h title)
  (zs-win (- zs-ax 2) (- zs-ay 14) (+ (* zs-s w) 4) (+ (* zs-s h) 16) title))
; repl's (line w x0 y0 x1 y1 1), (box w x0 y0 x1 y1 1), (text w x y s 1)
(define (zs-line x0 y0 x1 y1) (line (zs-px x0) (zs-py y0) (zs-px x1) (zs-py y1) 'ink 1.5))
(define (zs-box x0 y0 x1 y1 role)
  (rect (zs-px x0) (zs-py y0) (* zs-s (+ 1 (- x1 x0))) (* zs-s (+ 1 (- y1 y0))) role))
(define (zs-text x y s) (text (zs-px x) (+ (zs-py y) 12) s 'small))
