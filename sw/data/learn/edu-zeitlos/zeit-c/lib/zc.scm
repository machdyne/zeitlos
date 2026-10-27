; C on Zeitlos: boxes and rows of memory, as the figures draw them.

; a box w x h at (x, y) with words centred in it
(define (zc-box x y w h words)
  (frame x y w h 'ink 1.5)
  (text (+ x (/ w 2)) (+ y (/ h 2) 5) words 'centre 'body))

; the same box, filled: its words in paper
(define (zc-box-on x y w h words)
  (rect x y w h 'ink)
  (text (+ x (/ w 2)) (+ y (/ h 2) 5) words 'centre 'body 'paper))

; a row of n cells, each w wide and 26 high, from (x, y). `cells` is a
; list of the words in each; the cell numbered `hi` is filled (or none,
; with -1). The cell numbers are written under the row.
(define (zc-cells x y w cells hi)
  (let loop ((k 0) (cs cells))
    (if (pair? cs)
        (let ((cx (+ x (* k w))))
          (if (= k hi)
              (zc-box-on cx y w 26 (car cs))
              (zc-box cx y w 26 (car cs)))
          (text (+ cx (/ w 2)) (+ y 40) (fmt k 0) 'centre 'small)
          (loop (+ k 1) (cdr cs))))))

; bit i of n (bit 0 is worth 1)
(define (zc-bit n i) (modulo (floor (/ n (expt 2 i))) 2))

; n's low 8 bits as a row of boxes 30 wide from (x, y), the most
; significant on the left: a 1 is a filled box. Place values above.
(define (zc-bits x y n)
  (for-range 0 8
    (lambda (k)
      (let* ((i (- 7 k))
             (bx (+ x (* k 30)))
             (one (= 1 (zc-bit n i))))
        (if one (rect (+ bx 1) (+ y 1) 28 28 'ink))
        (frame bx y 30 30 'ink 1)
        (text (+ bx 15) (+ y 21) (if one "1" "0") 'centre 'body (if one 'paper 'ink))
        (text (+ bx 15) (- y 6) (fmt (expt 2 i) 0) 'centre 'small)))))
