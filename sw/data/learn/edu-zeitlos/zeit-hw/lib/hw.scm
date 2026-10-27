; Connecting things: connectors, bits and small circuits, as the
; figures draw them. Every name starts with hw- so that nothing clashes.

; bit i of n (bit 0 is worth 1)
(define (hw-bit n i) (modulo (floor (/ n (expt 2 i))) 2))

; n written with `digits` binary digits, most significant first
(define (hw-bin n digits)
  (let loop ((i (- digits 1)) (acc ""))
    (if (< i 0) acc
        (loop (- i 1) (string-append acc (if (= 1 (hw-bit n i)) "1" "0"))))))

; the n-th string of a list, counting from 0
(define (hw-nth l n) (if (= n 0) (car l) (hw-nth (cdr l) (- n 1))))

; A PMOD connector seen from above: pins 1-6 in the top row and, when
; rows is 2, pins 7-12 under them. Each pin is a box w units wide with
; its number inside; `labels` holds one string per pin, in pin order,
; drawn above the top row and under the bottom one. The pin numbered
; `hi` is filled; 0 fills none.
(define (hw-pmod x y w rows labels hi)
  (for-range 0 (* rows 6)
    (lambda (k)
      (let* ((p (+ k 1))
             (row (if (< k 6) 0 1))
             (col (modulo k 6))
             (bx (+ x (* col w)))
             (by (+ y (* row (+ w 26))))
             (cx (+ bx (/ w 2)))
             (on (= p hi)))
        (if on (rect (+ bx 2) (+ by 2) (- w 4) (- w 4) 'ink))
        (frame (+ bx 1) (+ by 1) (- w 2) (- w 2) 'ink 1)
        (text cx (+ by (/ w 2) 5) (fmt p 0) 'centre 'body (if on 'paper 'ink))
        (text cx (if (= row 0) (- by 6) (+ by w 14)) (hw-nth labels k) 'centre 'small)))))

; a resistor drawn as a box between two points on a level line
(define (hw-resistor x0 x1 y label)
  (let ((m (/ (+ x0 x1) 2)))
    (line x0 y (- m 18) y 'ink 1.5)
    (frame (- m 18) (- y 7) 36 14 'ink 1.5)
    (line (+ m 18) y x1 y 'ink 1.5)
    (text m (- y 12) label 'centre 'small)))

; the same, standing between two heights
(define (hw-resistor-v x y0 y1 label)
  (let ((m (/ (+ y0 y1) 2)))
    (line x y0 x (- m 18) 'ink 1.5)
    (frame (- x 7) (- m 18) 14 36 'ink 1.5)
    (line x (+ m 18) x y1 'ink 1.5)
    (text (+ x 12) (+ m 4) label 'small)))

; ground: three short bars under (x, y)
(define (hw-ground x y)
  (line (- x 12) y (+ x 12) y 'ink 1.5)
  (line (- x 8) (+ y 4) (+ x 8) (+ y 4) 'ink 1.5)
  (line (- x 4) (+ y 8) (+ x 4) (+ y 8) 'ink 1.5))

; a switch to ground under (x, y): closed when `closed` is true
(define (hw-switch x y closed)
  (line x y x (+ y 14) 'ink 1.5)
  (dot x (+ y 14))
  (if closed
      (line x (+ y 14) x (+ y 34) 'ink 2)
      (line x (+ y 34) (+ x 14) (+ y 18) 'ink 2))
  (dot x (+ y 34))
  (line x (+ y 34) x (+ y 44) 'ink 1.5)
  (hw-ground x (+ y 44)))

; a level shown as a word in a box: "high (1)" or "low (0)"
(define (hw-level x y high)
  (frame (- x 44) (- y 15) 88 22 'ink 1)
  (text x y (if high "high: 1" "low: 0") 'centre 'body 'bold))
