; The network: machines and messages, as the figures draw them.

; a machine: a box with its name in it, and a second line under the name
(define (zn-machine x y w h name sub)
  (rect x y w h 'paper)
  (frame x y w h 'ink 1.5)
  (text (+ x (/ w 2)) (+ y (/ h 2) (if (string=? sub "") 5 -2)) name 'centre 'body 'bold)
  (if (not (string=? sub ""))
      (text (+ x (/ w 2)) (+ y (/ h 2) 14) sub 'centre 'small)))

; a message: an arrow from (x0, y0) to (x1, y1), its words over the
; middle, above the higher end
(define (zn-message x0 y0 x1 y1 words)
  (arrow x0 y0 x1 y1 'ink 2)
  (text (/ (+ x0 x1) 2) (- (min y0 y1) 6) words 'centre 'small))

; text drawn in the 'body face, 9 units a character: where character i is
(define zn-cw 9)
(define (zn-col x0 i) (+ x0 (* i zn-cw)))

; a bracket under characters from i to j (j not included) of text drawn
; at x0, its baseline y, with a label under it
(define (zn-under x0 y i j label)
  (let ((a (zn-col x0 i)) (b (- (zn-col x0 j) 1)))
    (line a (+ y 6) a (+ y 12) 'ink 1.5)
    (line a (+ y 12) b (+ y 12) 'ink 1.5)
    (line b (+ y 6) b (+ y 12) 'ink 1.5)
    (text (/ (+ a b) 2) (+ y 28) label 'centre 'body 'bold)))
