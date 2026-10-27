; Timeless Computing: bits, as the figures draw them.

; bit i of n (bit 0 is worth 1)
(define (bit n i) (modulo (floor (/ n (expt 2 i))) 2))

; n written with `digits` binary digits, most significant first
(define (bin n digits)
  (let loop ((i (- digits 1)) (acc ""))
    (if (< i 0) acc
        (loop (- i 1) (string-append acc (if (= 1 (bit n i)) "1" "0"))))))

; one hex digit, 0 to 15
(define (hex-digit d) (substring "0123456789ABCDEF" d (+ d 1)))

; n's low `digits` bits as a row of boxes w units wide, the most
; significant on the left, from (x, y): a 1 is a filled box. With
; values, each box's place value above it. A gap after every four
; boxes when nibbles is true.
(define (bit-boxes x y digits n w values nibbles)
  (for-range 0 digits
    (lambda (k)
      (let* ((i (- digits 1 k))
             (bx (+ x (* k w) (if nibbles (* 8 (floor (/ k 4))) 0)))
             (cx (+ bx (/ w 2)))
             (one (= 1 (bit n i))))
        (if one (rect (+ bx 1) (+ y 1) (- w 2) (- w 2) 'ink))
        (frame bx y w w 'ink 1)
        (text cx (+ y (/ w 2) 6) (if one "1" "0") 'centre 'body (if one 'paper 'ink))
        (if values (text cx (- y 6) (fmt (expt 2 i) 0) 'centre 'small))))))

; the place values of n's 1 bits, as a sum: "128 + 8 + 4"
(define (bit-sum n digits)
  (let loop ((i (- digits 1)) (acc ""))
    (if (< i 0) (if (string=? acc "") "0" acc)
        (loop (- i 1)
              (if (= 1 (bit n i))
                  (string-append acc (if (string=? acc "") "" " + ") (fmt (expt 2 i) 0))
                  acc)))))
