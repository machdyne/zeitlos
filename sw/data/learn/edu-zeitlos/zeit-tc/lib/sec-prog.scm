; Timeless Computing, Security and Programs: figures' procedures.

; a XOR b, bit by bit, for numbers 0 to 255
(define (sp-xor a b)
  (let loop ((i 0) (acc 0))
    (if (= i 8) acc
        (loop (+ i 1) (if (= (bit a i) (bit b i)) acc (+ acc (expt 2 i)))))))

; a labelled row of 8 bit boxes (bit-boxes is in bits.scm)
(define (sp-row y label n)
  (text 8 (+ y 17) label 'small)
  (bit-boxes 100 y 8 n 24 #f #f))

; how long trying 2^b guesses takes at a billion a second, drawn
; centred on (160, y)
(define (sp-time b y)
  (let* ((s (exp (- (* b (log 2)) (* 9 (log 10)))))
         (yr (/ s 31557600)))
    (cond ((< s 1) (text 160 y "under a second" 'centre 'body 'bold))
          ((< s 120) (text 160 y (string-append (fmt s 0) " seconds") 'centre 'body 'bold))
          ((< s 7200) (text 160 y (string-append (fmt (/ s 60) 0) " minutes") 'centre 'body 'bold))
          ((< s 172800) (text 160 y (string-append (fmt (/ s 3600) 0) " hours") 'centre 'body 'bold))
          ((< yr 2) (text 160 y (string-append (fmt (/ s 86400) 0) " days") 'centre 'body 'bold))
          ((< yr 1000000) (text 160 y (string-append (fmt yr 0) " years") 'centre 'body 'bold))
          (else (text 142 y "about" 'right 'body 'bold)
                (math 172 y (string-append "10^" (fmt (floor (/ (log yr) (log 10))) 0)) 'ink)
                (text 202 y "years" 'left 'body 'bold)))))

; binary search for t in the sorted list a, after k comparisons:
; (lo hi last found), last the place compared last (or -1)
(define (sp-search a t k)
  (let loop ((i 0) (lo 0) (hi (- (length a) 1)) (last -1))
    (let ((mid (floor (/ (+ lo hi) 2))))
      (cond ((and (>= last 0) (= (list-ref a last) t)) (list lo hi last #t))
            ((or (= i k) (> lo hi)) (list lo hi last #f))
            ((< (list-ref a mid) t) (loop (+ i 1) (+ mid 1) hi mid))
            ((> (list-ref a mid) t) (loop (+ i 1) lo (- mid 1) mid))
            (else (loop (+ i 1) lo hi mid))))))

; the list a as a row of boxes w wide from (x, y): places lo..hi
; framed, the place m filled
(define (sp-boxes a x y w lo hi m)
  (for-range 0 (length a)
    (lambda (i)
      (let ((bx (+ x (* i w))) (in (and (>= i lo) (<= i hi))))
        (if (= i m) (rect bx y (- w 1) 24 'ink))
        (if (or in (= i m)) (frame bx y (- w 1) 24 'ink 1))
        (text (+ bx (/ w 2)) (+ y 17) (fmt (list-ref a i) 0) 'centre 'small
              (if (= i m) 'paper 'ink))))))
