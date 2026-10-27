; Zeitlos -- learn's figure library (docs/learn_figures.md).
;
; Built into learn and loaded once. Every figure can use these; a course
; adds its own in lib/*.scm. Units: the figure's 320x240, y down, angles
; in degrees counter-clockwise from the right.

(define pi 3.141592653589793)
(define (deg->rad d) (* d (/ pi 180)))
(define (rad->deg r) (* r (/ 180 pi)))
(define (lerp a b t) (+ a (* (- b a) t)))
(define (clamp x lo hi) (if (< x lo) lo (if (> x hi) hi x)))
(define (for-range a b f) (if (< a b) (begin (f a) (for-range (+ a 1) b f))))

; what does not move: drawn once, and again only when a control changes
; (it must come first in a figure; anything drawn before it, and it runs every frame)
(define-macro (still . forms) `(if (%still-begin) (begin ,@forms (%still-end))))

; a point the learner can drag: (drag 'p x y) -> (x y), where it is now
(define (drag name x y . r) (apply %drag name x y r) (list (%ctl-x name) (%ctl-y name)))

(define (label x y s . o) (apply text x y s 'centre o))
; a line at the foot: small, or tiny when small would not fit
(define (caption s)
  (text 160 232 s 'centre (if (<= (* 7 (string-length s)) 312) 'small 'tiny)))

; -- arcs and wedges --

; x y x y ... along a circle from degree a0 to a1
(define (arc-points cx cy r a0 a1)
  (let ((n (max 2 (ceiling (/ (abs (- a1 a0)) 5)))))
    (let loop ((i n) (acc '()))
      (if (< i 0) acc
          (let ((d (deg->rad (lerp a0 a1 (/ i n)))))
            (loop (- i 1) (cons (+ cx (* r (cos d))) (cons (- cy (* r (sin d))) acc))))))))

(define (wedge cx cy r a0 a1 role)
  (poly (cons cx (cons cy (arc-points cx cy r a0 a1))) role))

; a mark for an angle: its arc and, if given, a label beside it
(define (angle-mark cx cy r a0 a1 . lab)
  (arc cx cy r a0 a1 'ink 1)
  (if (pair? lab)
      (let ((m (deg->rad (/ (+ a0 a1) 2))))
        (text (+ cx (* (+ r 10) (cos m))) (+ (- cy (* (+ r 10) (sin m))) 4) (car lab) 'centre 'small))))

; -- fractions --

; a bar cut into `parts`, the first `shaded` filled with role
(define (frac-bar-at x y w h parts shaded role)
  (for-range 0 parts
    (lambda (i)
      (if (< i shaded)
          (let ((x0 (+ x (/ (* w i) parts))) (x1 (+ x (/ (* w (+ i 1)) parts))))
            (rect x0 y (- x1 x0) h role)))))
  (frame x y w h 'ink 1.5)
  ; a cut through a filled piece keeps a gap of paper each side, to be seen on one bit
  (for-range 1 parts
    (lambda (i)
      (let ((xi (+ x (/ (* w i) parts))))
        (if (<= i shaded) (line xi (+ y 1) xi (+ y h -1) 'paper 3))
        (line xi y xi (+ y h) 'ink 1)))))

; rows of bars, the same width, a fraction beside each: (frac-bar 0 4 3 'warm "3/4")
(define (frac-bar row parts shaded role . lab)
  (let ((y (+ 20 (* row 56))))
    (frac-bar-at 80 y 216 30 parts shaded role)
    (if (pair? lab) (math 40 (+ y 21) (car lab) 22))))

; a circle cut into `parts` from the top, clockwise
(define (frac-circle cx cy r parts shaded role)
  (for-range 0 parts
    (lambda (i)
      (if (< i shaded)
          (wedge cx cy r (- 90 (* 360 (/ i parts))) (- 90 (* 360 (/ (+ i 1) parts))) role))))
  (ring cx cy r 'ink 1.5)
  (if (> parts 1)
      (for-range 0 parts
        (lambda (i)
          (let ((d (deg->rad (- 90 (* 360 (/ i parts))))))
            (line cx cy (+ cx (* r (cos d))) (- cy (* r (sin d))) 'ink 1))))))

; -- number lines --

(define %nl (list 20 300 120 0 10))

(define (near-whole? q) (< (abs (- q (round q))) 0.000001))

; (number-line x0 x1 y from to [tick] [label-every])
(define (number-line x0 x1 y from to . opt)
  (let* ((step (if (pair? opt) (car opt) 1))
         (lstep (if (and (pair? opt) (pair? (cdr opt))) (cadr opt) step)))
    (set! %nl (list x0 x1 y from to))
    (line (- x0 6) y (+ x1 6) y 'ink 1.5)
    (let loop ((v from))
      (if (<= v (+ to 0.000001))
          (let ((x (nl-x v)) (big (near-whole? (/ (- v from) lstep))))
            (line x (- y (if big 5 3)) x (+ y (if big 5 3)) 'ink 1)
            (if big (text x (+ y 17) v 'centre 'small))
            (loop (+ v step)))))))

(define (nl-x v)
  (let ((x0 (car %nl)) (x1 (cadr %nl)) (from (cadddr %nl)) (to (car (cddddr %nl))))
    (lerp x0 x1 (/ (- v from) (- to from)))))

(define (nl-y) (caddr %nl))

; a point on the line, and its label above
(define (nl-mark v role . lab)
  (circle (nl-x v) (nl-y) 4 role)
  (ring (nl-x v) (nl-y) 4 'ink 1)
  (if (pair? lab) (text (nl-x v) (- (nl-y) 10) (car lab) 'centre 'small)))

; a hop from a to b, above the line, with a label over it
(define (nl-jump a b role . lab)
  (let* ((xa (nl-x a)) (xb (nl-x b)) (cx (/ (+ xa xb) 2)) (r (/ (abs (- xb xa)) 2))
         (y (nl-y)) (dir (if (> xb xa) 1 -1)))
    (arc cx y r 0 180 role 1.5)
    (arrow xb (- y 10) xb (- y 1) role 1.5)
    (if (pair? lab) (text cx (- y r 5) (car lab) 'centre 'small))))

; -- graphs --

(define %gl 40) (define %gb 208) (define %gx0 0) (define %gx1 10) (define %gy0 0) (define %gy1 10)
(define %gsx 26.4) (define %gsy 19.2)

; the graph's place and its ranges: (graph left top w h xmin xmax ymin ymax)
(define (graph left top w h xmin xmax ymin ymax)
  (set! %gl left) (set! %gb (+ top h))
  (set! %gx0 xmin) (set! %gx1 xmax) (set! %gy0 ymin) (set! %gy1 ymax)
  (set! %gsx (/ w (- xmax xmin))) (set! %gsy (/ h (- ymax ymin))))

(define (gx x) (+ %gl (* %gsx (- x %gx0))))
(define (gy y) (- %gb (* %gsy (- y %gy0))))

; axes through 0 (or along the edges), ticks every xstep and ystep
(define (graph-axes xstep ystep . names)
  (let* ((x0 %gx0) (x1 %gx1) (y0 %gy0) (y1 %gy1)
         (ax (if (and (<= y0 0) (>= y1 0)) 0 y0))
         (ay (if (and (<= x0 0) (>= x1 0)) 0 x0)))
    (line (gx x0) (gy ax) (gx x1) (gy ax) 'ink 1.5)
    (line (gx ay) (gy y0) (gx ay) (gy y1) 'ink 1.5)
    (let loop ((v (* xstep (ceiling (/ x0 xstep)))))
      (if (<= v (+ x1 0.000001))
          (begin
            (line (gx v) (- (gy ax) 3) (gx v) (+ (gy ax) 3) 'ink 1)
            (if (not (= v ay)) (text (gx v) (+ (gy ax) 15) v 'centre 'tiny))
            (loop (+ v xstep)))))
    (let loop ((v (* ystep (ceiling (/ y0 ystep)))))
      (if (<= v (+ y1 0.000001))
          (begin
            (line (- (gx ay) 3) (gy v) (+ (gx ay) 3) (gy v) 'ink 1)
            (if (not (= v ax)) (text (- (gx ay) 6) (+ (gy v) 3) v 'right 'tiny))
            (loop (+ v ystep)))))
    (if (pair? names)
        (begin
          (text (gx x1) (- (gy ax) 6) (car names) 'right 'small)
          (if (pair? (cdr names)) (text (+ (gx ay) 6) (+ (gy y1) 10) (cadr names) 'small))))))

; y = (f x) across the graph, broken where it leaves it
(define (plot f role . width)
  (let* ((w (if (pair? width) (car width) 2)) (n 80) (dx (/ (- %gx1 %gx0) n)))
    (define (flush pts) (if (and (pair? pts) (pair? (cddr pts))) (path pts role w)))
    ; from the right end back, so the list comes out in order
    (let loop ((i n) (pts '()))
      (if (< i 0) (flush pts)
          (let* ((x (+ %gx0 (* i dx))) (y (f x)))
            (if (and (>= y %gy0) (<= y %gy1))
                (loop (- i 1) (cons (gx x) (cons (gy y) pts)))
                (begin (flush pts) (loop (- i 1) '()))))))))

(define (plot-point x y role . lab)
  (circle (gx x) (gy y) 3.5 role)
  (ring (gx x) (gy y) 3.5 'ink 1)
  (if (pair? lab) (text (+ (gx x) 7) (- (gy y) 6) (car lab) 'small)))

; bars from a list of ("name" value): (bar-chart '(("Mon" 3) ("Tue" 5)) 'cool)
(define (bar-chart items role . top)
  (let* ((n (length items))
         (most (if (pair? top) (car top) (apply max (map cadr items))))
         (w (/ 240 n)))
    (line 40 200 290 200 'ink 1.5)
    (let loop ((l items) (i 0))
      (if (pair? l)
          (let* ((v (cadr (car l))) (h (* 160 (/ v most))) (x (+ 46 (* i w))))
            (rect x (- 200 h) (- w 10) h role)
            (frame x (- 200 h) (- w 10) h 'ink 1)
            (text (+ x (/ (- w 10) 2)) 214 (car (car l)) 'centre 'small)
            (text (+ x (/ (- w 10) 2)) (- 200 h 4) v 'centre 'small)
            (loop (cdr l) (+ i 1)))))))

; -- counting and measuring --

(define (dot-array x y rows cols gap role)
  (for-range 0 rows
    (lambda (r) (for-range 0 cols (lambda (c) (circle (+ x (* c gap)) (+ y (* r gap)) (* gap 0.32) role))))))

(define (grid x y cols rows size . role)
  (let ((ro (if (pair? role) (car role) 'ink)))
    (for-range 0 (+ cols 1) (lambda (i) (line (+ x (* i size)) y (+ x (* i size)) (+ y (* rows size)) ro 1)))
    (for-range 0 (+ rows 1) (lambda (i) (line x (+ y (* i size)) (+ x (* cols size)) (+ y (* i size)) ro 1)))))

; place value: hundreds (flats), tens (rods), ones (cubes), 3 units a cube
(define (base-ten x y hundreds tens ones)
  (let loop ((i 0) (x x))
    (if (< i hundreds)
        (begin (rect x y 30 30 'warm) (grid x y 10 10 3) (loop (+ i 1) (+ x 36)))
        (let loop2 ((j 0) (x x))
          (if (< j tens)
              (begin (rect x y 3 30 'cool) (grid x y 1 10 3) (loop2 (+ j 1) (+ x 7)))
              (for-range 0 ones
                (lambda (k) (rect (+ x 2 (* 7 (modulo k 3))) (+ y (* 7 (quotient k 3))) 4 4 'good)
                            (frame (+ x 2 (* 7 (modulo k 3))) (+ y (* 7 (quotient k 3))) 4 4 'ink 1))))))))

; a ruler: len units, px a unit, a number every label-every units
(define (ruler x y len px . every)
  (let ((e (if (pair? every) (car every) 1)))
    (frame x y (* len px) 22 'ink 1)
    (for-range 0 (+ (* len 2) 1)
      (lambda (h)
        (let ((xx (+ x (* h (/ px 2)))) (whole (even? h)))
          (line xx y xx (+ y (if whole 9 5)) 'ink 1)
          (if (and whole (= 0 (modulo (/ h 2) e))) (text xx (+ y 19) (/ h 2) 'centre 'tiny)))))))

(define (tally x y n)
  (for-range 0 n
    (lambda (i)
      (let* ((g (quotient i 5)) (k (modulo i 5)) (gx (+ x (* g 34))))
        (if (< k 4)
            (line (+ gx (* k 6)) y (+ gx (* k 6)) (+ y 24) 'ink 2)
            (line (- gx 3) (+ y 20) (+ gx 22) (+ y 4) 'ink 2))))))

; an analogue clock: its face (still), then its hands at h:m
(define (clock-dial cx cy r)
  (circle cx cy r 'paper)
  (ring cx cy r 'ink 2)
  (for-range 0 60
    (lambda (i)
      (let* ((d (deg->rad (- 90 (* i 6)))) (c (cos d)) (s (sin d))
             (big (= 0 (modulo i 5))) (in (- r (if big 8 4))))
        (line (+ cx (* in c)) (- cy (* in s)) (+ cx (* r c)) (- cy (* r s)) 'ink (if big 2 1)))))
  (for-range 1 13
    (lambda (i)
      (let* ((d (deg->rad (- 90 (* i 30)))) (in (- r 18)))
        (text (+ cx (* in (cos d))) (+ (- cy (* in (sin d))) 4) i 'centre 'small)))))

(define (clock-hands cx cy r h m . opt)
  (let ((hd (deg->rad (- 90 (* 30 (+ (modulo h 12) (/ m 60))))))
        (md (deg->rad (- 90 (* 6 m)))))
    (line cx cy (+ cx (* r 0.5 (cos hd))) (- cy (* r 0.5 (sin hd))) 'ink 4)
    (line cx cy (+ cx (* r 0.8 (cos md))) (- cy (* r 0.8 (sin md))) 'ink 2)
    ; a second hand, when the seconds are given: thin, with a short tail
    (if (pair? opt)
        (let* ((sd (deg->rad (- 90 (* 6 (car opt))))) (c (cos sd)) (s (sin sd)))
          (line (- cx (* r 0.15 c)) (+ cy (* r 0.15 s)) (+ cx (* r 0.9 c)) (- cy (* r 0.9 s)) 'warm 1)))
    (circle cx cy 3 'ink)))

; a clock that keeps time from h:m:00 when the step is shown, its second
; hand ticking once a second (a figure that moves, with the dial in `still`)
(define (clock-ticking cx cy r h m)
  (let ((t (floor (seconds))))
    (clock-hands cx cy r (+ h (floor (/ (+ m (/ t 60)) 60))) (modulo (+ m (/ t 60)) 60) (modulo t 60))))

(define (clock-face cx cy r h m)
  (clock-dial cx cy r)
  (clock-hands cx cy r h m))
