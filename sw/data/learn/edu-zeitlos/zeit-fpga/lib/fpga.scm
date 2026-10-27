; The FPGA (zeit-fpga): procedures for this course's figures.
; Every name starts with fp- so as not to clash with the library.

; one hex digit, 0 to 15
(define (fp-hex d) (substring "0123456789ABCDEF" d (+ d 1)))

; a box with a centred label (one or two lines)
(define (fp-box x y w h label)
  (frame x y w h 'ink 1.2)
  (text (+ x (/ w 2)) (+ y (/ h 2) 5) label 'centre 'small))

(define (fp-box2 x y w h l1 l2)
  (frame x y w h 'ink 1.2)
  (text (+ x (/ w 2)) (+ y (/ h 2) -2) l1 'centre 'small)
  (text (+ x (/ w 2)) (+ y (/ h 2) 11) l2 'centre 'tiny))

; a shaded box with a label: the part a figure points at
(define (fp-box-on x y w h label)
  (rect x y w h 'cool)
  (frame x y w h 'ink 2)
  (text (+ x (/ w 2)) (+ y (/ h 2) 5) label 'centre 'small 'bold))

; 2 to the n, for whole n >= 0, without floating point
(define (fp-pow2 n)
  (let loop ((k 0) (acc 1))
    (if (>= k n) acc (loop (+ k 1) (* acc 2)))))

; a horizontal bar: total width w stands for `total`, filled to `used`
(define (fp-bar x y w h used total role)
  (rect x y (* w (/ used total)) h role)
  (frame x y w h 'ink 1))
