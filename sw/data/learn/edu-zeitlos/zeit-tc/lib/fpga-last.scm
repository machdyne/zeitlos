; Timeless Computing, units 12-17: LUTs and waveforms.

; bit i of n (bit 0 is worth 1)
(define (fl-bit n i) (modulo (floor (/ n (expt 2 i))) 2))

; n's low `digits` bits, most significant first: "0110"
(define (fl-bin n digits)
  (let loop ((i (- digits 1)) (acc ""))
    (if (< i 0) acc
        (loop (- i 1) (string-append acc (if (= 1 (fl-bit n i)) "1" "0"))))))

; a 16-bit number as four hex digits
(define (fl-hex16 n)
  (let loop ((i 3) (acc ""))
    (if (< i 0) acc
        (let ((d (modulo (floor (/ n (expt 16 i))) 16)))
          (loop (- i 1) (string-append acc (substring "0123456789ABCDEF" d (+ d 1))))))))

; a 4-input LUT holding `init`, its inputs a b c d (0 or 1): the box,
; the inputs, the 16 stored bits as two columns of 8 rows, the row the
; inputs address framed, and the output: the bit stored at that address.
(define (fl-lut init a b c d)
  (let* ((addr (+ a (* 2 b) (* 4 c) (* 8 d)))
         (out (fl-bit init addr)))
    (frame 44 44 56 116 'ink 1.5)
    (text 72 106 "LUT" 'centre 'body 'bold)
    (for-range 0 4
      (lambda (k)
        (let ((y (+ 62 (* 26 k))))
          (line 26 y 44 y 'ink 1)
          (text 4 (+ y 4) (list-ref '("a" "b" "c" "d") k) 'small)
          (text 16 (+ y 4) (fmt (list-ref (list a b c d) k) 0) 'small 'bold))))
    (arrow 100 102 128 102 'ink 1)
    (text 114 94 (fmt out 0) 'centre 'big 'bold)
    (text 72 182 (string-append "address " (fmt addr 0)) 'centre 'small)
    (text 72 198 (string-append "dcba = " (fl-bin addr 4)) 'centre 'small)
    (for-range 0 2
      (lambda (col)
        (let ((x (+ 150 (* 86 col))))
          (text (+ x 10) 30 "addr" 'centre 'tiny)
          (text (+ x 46) 30 "bit" 'centre 'tiny)
          (for-range 0 8
            (lambda (r)
              (let ((n (+ r (* 8 col))) (y (+ 50 (* 20 r))))
                (text (+ x 10) y (fmt n 0) 'centre 'small)
                (text (+ x 46) y (fmt (fl-bit init n) 0) 'centre 'small 'bold)
                (if (= n addr) (frame (- x 6) (- y 14) 70 19 'ink 2))))))))
    (text 236 214 (string-append "contents 0x" (fl-hex16 init)) 'centre 'small)
    out))

; a digital waveform: `vals` a list of 0s and 1s, each `step` units
; wide, from x, high at y and low at y + h
(define (fl-wave x y h step vals role)
  (let loop ((v vals) (px x) (last -1) (pts '()))
    (if (null? v)
        (path (reverse (cons (+ y (if (= last 1) 0 h)) (cons px pts))) role 1.5)
        (let ((py (+ y (if (= (car v) 1) 0 h))))
          (loop (cdr v) (+ px step) (car v)
                (if (= (car v) last)
                    pts
                    (if (< last 0)
                        (cons py (cons px pts))
                        (cons py (cons px (cons (+ y (if (= last 1) 0 h)) (cons px pts)))))))))))

; the list (f 0) (f 1) ... (f (- n 1))
(define (fl-list n f)
  (let loop ((i (- n 1)) (acc '()))
    (if (< i 0) acc (loop (- i 1) (cons (f i) acc)))))

; a clock from x0 to x1: low until `start`, then turning every `half`
(define (fl-clk x0 x1 y h half start role)
  (let loop ((e start) (hi #f) (pts (list (+ y h) x0)))
    (if (>= e x1)
        (path (reverse (cons (if hi y (+ y h)) (cons x1 pts))) role 1.5)
        (loop (+ e half) (not hi)
              (cons (if hi (+ y h) y) (cons e (cons (if hi y (+ y h)) (cons e pts))))))))
