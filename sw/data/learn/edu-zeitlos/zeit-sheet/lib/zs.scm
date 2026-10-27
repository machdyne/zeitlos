; Records and numbers: a little sheet, drawn the way sheet draws one --
; column letters over the cells, row numbers to their left, text to the
; left of its cell and numbers to the right.
;
;   (zs-place x y cw rh c0 r0)   where the grid is: its first cell's
;                                corner, a cell's size, the first
;                                column (0 is A) and the first row
;   (zs-fill ca ra cb rb role)   shade a block of cells (before zs-sheet)
;   (zs-sheet data)              the headers and the cells: data is a
;                                list of rows, each a list of strings
;                                ("'$12" is text that looks like a number)
;   (zs-box ca ra cb rb)         a heavy frame round a block (the cursor
;                                or a selection)
;   (zs-bar x y w ref content)   the edit bar: a reference and a text

(define zs-x 40) (define zs-y 60) (define zs-cw 64) (define zs-rh 18)
(define zs-c0 0) (define zs-r0 1)

(define (zs-letter c) (substring "ABCDEFGHIJKLMNOPQRSTUVWXYZ" c (+ c 1)))
(define (zs-ref c r) (string-append (zs-letter c) (fmt r 0)))

(define (zs-place x y cw rh c0 r0)
  (set! zs-x x) (set! zs-y y) (set! zs-cw cw) (set! zs-rh rh)
  (set! zs-c0 c0) (set! zs-r0 r0))

; a cell's left and top edges
(define (zs-cx c) (+ zs-x (* (- c zs-c0) zs-cw)))
(define (zs-cy r) (+ zs-y (* (- r zs-r0) zs-rh)))

; a number, an error or money is drawn to the right of its cell
(define (zs-right? s)
  (and (> (string-length s) 0)
       (let ((f (string-ref s 0)))
         (or (char-numeric? f) (char=? f (string-ref "-" 0)) (char=? f (string-ref "$" 0))
             (char=? f (string-ref "#" 0)) (char=? f (string-ref "." 0))))))

(define (zs-fill ca ra cb rb role)
  (rect (zs-cx ca) (zs-cy ra) (* (+ 1 (- cb ca)) zs-cw) (* (+ 1 (- rb ra)) zs-rh) role))

(define (zs-box ca ra cb rb)
  (frame (- (zs-cx ca) 1) (- (zs-cy ra) 1) (+ 2 (* (+ 1 (- cb ca)) zs-cw)) (+ 2 (* (+ 1 (- rb ra)) zs-rh)) 'ink 2.5))

; one cell's text, cut to fit (sheet cuts text; numbers are never cut,
; and the lessons' numbers are written to fit)
; text that would look like a number is written with a ' in front, as
; in sheet: it is drawn at the left, without the '
(define (zs-text c r s)
  (let* ((forced (char=? (string-ref s 0) (string-ref "'" 0)))
         (s (if forced (substring s 1 (string-length s)) s))
         (most (floor (/ (- zs-cw 4) 7)))
         (t (if (> (string-length s) most) (substring s 0 most) s)))
    (if (and (not forced) (zs-right? t))
        (text (- (+ (zs-cx c) zs-cw) 3) (+ (zs-cy r) (- zs-rh 5)) t 'small 'right)
        (text (+ (zs-cx c) 3) (+ (zs-cy r) (- zs-rh 5)) t 'small))))

(define (zs-sheet data)
  (let* ((rows (length data)) (cols (length (car data)))
         (x0 (- zs-x 22)) (y0 (- zs-y zs-rh))
         (x1 (+ zs-x (* cols zs-cw))) (y1 (+ zs-y (* rows zs-rh))))
    ; the headers
    (frame x0 y0 (- x1 x0) (- y1 y0) 'ink 1)
    (line x0 zs-y x1 zs-y 'ink 1)
    (line zs-x y0 zs-x y1 'ink 1)
    (for-range 0 cols
      (lambda (i)
        (let ((c (+ zs-c0 i)))
          (if (> i 0) (line (zs-cx c) y0 (zs-cx c) y1 'grey 1))
          (text (+ (zs-cx c) (/ zs-cw 2)) (- zs-y 5) (zs-letter c) 'small 'centre 'bold))))
    (for-range 0 rows
      (lambda (i)
        (let ((r (+ zs-r0 i)))
          (if (> i 0) (line x0 (zs-cy r) x1 (zs-cy r) 'grey 1))
          (text (- zs-x 4) (+ (zs-cy r) (- zs-rh 5)) (fmt r 0) 'small 'right))))
    ; the cells
    (let loop ((rl data) (r zs-r0))
      (if (pair? rl)
          (begin
            (let cl ((l (car rl)) (c zs-c0))
              (if (pair? l)
                  (begin
                    (if (> (string-length (car l)) 0) (zs-text c r (car l)))
                    (cl (cdr l) (+ c 1)))))
            (loop (cdr rl) (+ r 1)))))))

; the edit bar: the reference box, then what the cell holds
(define (zs-bar x y w ref content)
  (frame x y w 20 'ink 1)
  (line (+ x 52) y (+ x 52) (+ y 20) 'ink 1)
  (text (+ x 5) (+ y 14) ref 'small 'bold)
  (text (+ x 57) (+ y 14) content 'small))

; a chart of one or two series over a few categories, in patterns as
; sheet draws them: kind 0 bars, 1 stacked bars, 2 lines, 3 a pie (the
; first series only). names: the categories; a, b: the series (b may be
; '()); top: the axis's top value.
(define (zs-chart kind names a b top)
  (let* ((n (length names)) (x0 40) (x1 250) (y0 40) (y1 190)
         (w (/ (- x1 x0) n))
         (yv (lambda (v) (- y1 (* (- y1 y0) (/ v top))))))
    (if (= kind 3)
        (let ((tot (apply + a)))
          (let loop ((l a) (i 0) (from 90))
            (if (pair? l)
                (let ((to (- from (* 360 (/ (car l) tot)))))
                  (wedge 145 115 75 to from (list-ref '(ink cool warm good) (modulo i 4)))
                  (let ((m (deg->rad (/ (+ from to) 2))))
                    (text (+ 145 (* 92 (cos m))) (+ 119 (* -92 (sin m))) (list-ref names i) 'small 'centre))
                  (loop (cdr l) (+ i 1) to))))
          (ring 145 115 75 'ink 1))
        (begin
          (line x0 y1 x1 y1 'ink 1.5)
          (line x0 y0 x0 y1 'ink 1.5)
          (text (- x0 4) (+ y0 4) (fmt top 0) 'small 'right)
          (text (- x0 4) (+ y1 4) "0" 'small 'right)
          (line (- x0 3) y0 x0 y0 'ink 1)
          (let loop ((i 0) (la a) (lb b) (pa #f) (pb #f))
            (if (< i n)
                (let* ((va (car la)) (vb (if (pair? lb) (car lb) 0))
                       (cx (+ x0 (* i w) (/ w 2))))
                  (text cx (+ y1 14) (list-ref names i) 'small 'centre)
                  (cond
                    ((= kind 0)
                     (rect (- cx 18) (yv va) 17 (- y1 (yv va)) 'ink)
                     (if (pair? lb)
                         (begin (rect (+ cx 1) (yv vb) 17 (- y1 (yv vb)) 'cool)
                                (frame (+ cx 1) (yv vb) 17 (- y1 (yv vb)) 'ink 1))))
                    ((= kind 1)
                     (rect (- cx 12) (yv va) 24 (- y1 (yv va)) 'ink)
                     (if (pair? lb)
                         (begin (rect (- cx 12) (yv (+ va vb)) 24 (- (yv va) (yv (+ va vb))) 'cool)
                                (frame (- cx 12) (yv (+ va vb)) 24 (- (yv va) (yv (+ va vb))) 'ink 1))))
                    (else
                     (if pa (line (car pa) (cadr pa) cx (yv va) 'ink 1.5))
                     (rect (- cx 3) (- (yv va) 3) 7 7 'ink)
                     (if (pair? lb)
                         (begin
                           (if pb (line (car pb) (cadr pb) cx (yv vb) 'ink 1))
                           (frame (- cx 3) (- (yv vb) 3) 7 7 'ink 1)))))
                  (loop (+ i 1) (cdr la) (if (pair? lb) (cdr lb) lb)
                        (list cx (yv va)) (list cx (yv vb))))))))))

; the legend, at the right: a pattern or marker and a name for each
(define (zs-legend kind sa sb)
  (if (< kind 2)
      (begin (rect 262 60 12 10 'ink) (text 278 69 sa 'small)
             (if sb (begin (rect 262 80 12 10 'cool) (frame 262 80 12 10 'ink 1) (text 278 89 sb 'small))))
      (if (= kind 2)
          (begin (rect 265 61 7 7 'ink) (text 278 69 sa 'small)
                 (if sb (begin (frame 265 81 7 7 'ink 1) (text 278 89 sb 'small)))))))

; a number as sheet's general format shows it: as short as it can be
; (up to 4 places here, enough for the lessons' numbers)
(define (zs-g v)
  (let loop ((s (fmt v 4)))
    (let ((n (string-length s)))
      (cond ((char=? (string-ref s (- n 1)) (string-ref "0" 0)) (loop (substring s 0 (- n 1))))
            ((char=? (string-ref s (- n 1)) (string-ref "." 0)) (substring s 0 (- n 1)))
            (else s)))))
