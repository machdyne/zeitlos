; Timeless Computing, Electricity: circuit symbols for the figures.

; a resistor from (x0,y0) to (x1,y1) as a zigzag; its label above
; (across) or to the right (upright)
(define (el-res x0 y0 x1 y1 lab)
  (let* ((dx (- x1 x0)) (dy (- y1 y0))
         (l (sqrt (+ (* dx dx) (* dy dy)))) (px (- (/ dy l))) (py (/ dx l)))
    (define (pt k)
      (let ((t (+ 0.2 (* k 0.1)))
            (o (if (or (= k 0) (= k 6)) 0 (if (odd? k) 6 -6))))
        (list (+ x0 (* dx t) (* px o)) (+ y0 (* dy t) (* py o)))))
    (path (append (list x0 y0) (pt 0) (pt 1) (pt 2) (pt 3) (pt 4) (pt 5) (pt 6)
                  (list x1 y1)) 'ink 1.5)
    (if (= dy 0)
        (text (/ (+ x0 x1) 2) (- y0 11) lab 'centre 'small)
        (text (+ x0 12) (+ (/ (+ y0 y1) 2) 4) lab 'small))))

; a switch from (x0,y0) to (x1,y1): its blade touches the far contact
; when closed, and stands away from it when open
(define (el-switch x0 y0 x1 y1 closed)
  (let ((dx (- x1 x0)) (dy (- y1 y0)))
    (dot x0 y0) (dot x1 y1)
    (if closed (line x0 y0 x1 y1 'ink 2)
        (line x0 y0 (+ x0 (* 0.87 dx) (* 0.5 dy)) (- (+ y0 (* 0.87 dy)) (* 0.5 dx)) 'ink 2))))

; a lamp at (cx,cy): a circle with a cross; rays when it is lit
(define (el-lamp cx cy on)
  (circle cx cy 12 'paper)
  (ring cx cy 12 'ink 1.5)
  (line (- cx 8) (- cy 8) (+ cx 8) (+ cy 8) 'ink 1)
  (line (- cx 8) (+ cy 8) (+ cx 8) (- cy 8) 'ink 1)
  (if on
      (for-each (lambda (d)
                  (line (+ cx (* 16 (car d))) (+ cy (* 16 (cadr d)))
                        (+ cx (* 24 (car d))) (+ cy (* 24 (cadr d))) 'ink 1.5))
                '((1 0) (-1 0) (0 1) (0 -1) (0.7 0.7) (-0.7 0.7) (0.7 -0.7) (-0.7 -0.7)))))

; a cell, upright at x from y0 (+, the long plate) to y1
(define (el-cell x y0 y1 lab)
  (let ((m (/ (+ y0 y1) 2)))
    (line x y0 x (- m 4) 'ink 1.5)
    (line x (+ m 4) x y1 'ink 1.5)
    (line (- x 14) (- m 4) (+ x 14) (- m 4) 'ink 2)
    (line (- x 7) (+ m 4) (+ x 7) (+ m 4) 'ink 4)
    (text (- x 18) (- m 4) "+" 'small 'right)
    (text (+ x 18) (+ m 5) lab 'small)))

; the ground symbol, hanging from (x,y)
(define (el-ground x y)
  (line x (- y 6) x y 'ink 1.5)
  (line (- x 10) y (+ x 10) y 'ink 1.5)
  (line (- x 6) (+ y 4) (+ x 6) (+ y 4) 'ink 1.5)
  (line (- x 2) (+ y 8) (+ x 2) (+ y 8) 'ink 1.5))

; an LED from x0 to x1 at y, current flowing to the right; rays when lit
(define (el-led x0 y x1 on)
  (let ((m (/ (+ x0 x1) 2)))
    (line x0 y (- m 8) y 'ink 1.5)
    (line (+ m 8) y x1 y 'ink 1.5)
    (poly (list (- m 8) (- y 9) (- m 8) (+ y 9) (+ m 8) y) 'ink)
    (line (+ m 8) (- y 9) (+ m 8) (+ y 9) 'ink 2)
    (if on (begin (arrow (+ m 2) (- y 13) (+ m 10) (- y 21) 'ink 1)
                  (arrow (+ m 9) (- y 11) (+ m 17) (- y 19) 'ink 1)))))

; amps as milliamps, "4.81 mA"
(define (el-ma a) (string-append (fmt (* a 1000) 2) " mA"))
