; Programming in BASIC: the figures' own procedures (all named zb-...).

; The BASIC screen, 320 x 240 pixels, drawn three quarters size with
; its top left corner at (zb-x0, zb-y0). It is white on black, as the
; real one: the screen is ink and what BASIC draws is paper.
(define zb-x0 40)
(define zb-y0 20)
(define zb-k 0.75)
(define (zb-sx x) (+ zb-x0 (* zb-k x)))
(define (zb-sy y) (+ zb-y0 (* zb-k y)))
(define (zb-screen) (rect zb-x0 zb-y0 240 180 'ink))

; a figure position back to the pixel under it, kept on the screen
(define (zb-px u) (clamp (round (/ (- u zb-x0) zb-k)) 0 319))
(define (zb-py u) (clamp (round (/ (- u zb-y0) zb-k)) 0 239))

; a whole number as text
(define (zb-n v) (fmt v 0))

; the k-th item of a list, from 0
(define (zb-nth l k) (if (= k 0) (car l) (zb-nth (cdr l) (- k 1))))
(define (zb-len l) (if (null? l) 0 (+ 1 (zb-len (cdr l)))))

; A program running, one line at a time. lines: the program's lines.
; states: a list of (line "variables" (printed ...) "what happens"),
; where line is the index of the line just run, or -1 once the program
; has ended. One state every secs seconds, then the program again.
(define (zb-trace lines states secs)
  (let* ((n (zb-len states))
         (k (modulo (floor (/ (seconds) secs)) n))
         (s (zb-nth states k))
         (at (car s))
         (vars (car (cdr s)))
         (out (car (cdr (cdr s))))
         (note (car (cdr (cdr (cdr s))))))
    (let loop ((i 0) (l lines))
      (if (not (null? l))
          (begin
            (text 36 (+ 22 (* 20 i)) (car l) 'body (if (= i at) 'bold 'ink))
            (loop (+ i 1) (cdr l)))))
    (if (>= at 0) (arrow 6 (+ 17 (* 20 at)) 28 (+ 17 (* 20 at)) 'ink 2))
    (text 20 118 "Variables" 'small)
    (text 20 146 vars 'big)
    (text 170 98 "Screen" 'small)
    (rect 170 104 140 96 'ink)
    (let loop ((i 0) (l out))
      (if (not (null? l))
          (begin
            (text 176 (+ 120 (* 13 i)) (car l) 'small 'paper)
            (loop (+ i 1) (cdr l)))))
    (text 160 226 note 'small 'centre)))
