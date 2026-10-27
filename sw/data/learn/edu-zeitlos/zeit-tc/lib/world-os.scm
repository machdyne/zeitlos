; Timeless Computing, units 7-9 (the world, operating systems,
; communication): signals and boxes, as their figures draw them.

; bit i of n (bit 0 is worth 1)
(define (wo-bit n i) (modulo (floor (/ n (expt 2 i))) 2))

; a digital signal as one line: levels is a list of 1 (high, drawn at
; y) and 0 (low, at y + h), each w units wide, from x
(define (wo-wave x y w h levels role)
  (let loop ((l levels) (px x) (acc '()))
    (if (null? l)
        (path (reverse acc) role 1.5)
        (let ((ly (if (= (car l) 1) y (+ y h))))
          (loop (cdr l) (+ px w) (cons ly (cons (+ px w) (cons ly (cons px acc)))))))))

; the byte n as a UART sends it, slots w wide from x, the wire high at
; y and low at y + 30: idle (high), the start bit (low), the data bits
; b0 to b7 (least significant first), the stop bit (high), idle
(define (wo-uart x y w n)
  (let loop ((i 7) (acc '(1 1)))
    (if (>= i 0)
        (loop (- i 1) (cons (wo-bit n i) acc))
        (wo-wave x y w 30 (cons 1 (cons 0 acc)) 'ink)))
  (for-range 1 11
    (lambda (k)
      (let ((cx (+ x (* k w) (/ w 2))))
        (line (+ x (* k w)) (- y 18) (+ x (* k w)) (+ y 44) 'grey 0.5)
        (cond ((= k 1) (text cx (+ y 46) "start" 'centre 'tiny))
              ((= k 10) (text cx (+ y 46) "stop" 'centre 'tiny))
              (else
               (text cx (+ y 46) (string-append "b" (fmt (- k 2) 0)) 'centre 'small)
               (text cx (- y 6) (fmt (wo-bit n (- k 2)) 0) 'centre 'small 'bold))))))
  (line (+ x (* 11 w)) (- y 18) (+ x (* 11 w)) (+ y 44) 'grey 0.5)
  (text (+ x (/ w 2)) (- y 6) "idle" 'centre 'tiny)
  (text (+ x (* 11.5 w)) (- y 6) "idle" 'centre 'tiny))

; one bit as a box w square at (x, y): a 1 is filled
(define (wo-bitbox x y w b)
  (if (= b 1) (rect (+ x 1) (+ y 1) (- w 2) (- w 2) 'ink))
  (frame x y w w 'ink 1)
  (text (+ x (/ w 2)) (+ y (/ w 2) 6) (if (= b 1) "1" "0") 'centre 'body (if (= b 1) 'paper 'ink)))

; a box with words in its middle
(define (wo-box x y w h s . o)
  (frame x y w h 'ink 1)
  (apply text (+ x (/ w 2)) (+ y (/ h 2) 4) s 'centre o))

; a filled box with words in paper colour (the one in use)
(define (wo-onbox x y w h s . o)
  (rect x y w h 'ink)
  (apply text (+ x (/ w 2)) (+ y (/ h 2) 4) s 'centre 'paper o))
