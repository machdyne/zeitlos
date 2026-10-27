; Timeless Computing: the Computer and Memory units' figures.

; n (0 or more) as d hex digits, upper case
(define (cp-hex n d)
  (let loop ((i 0) (n n) (s ""))
    (if (= i d) s
        (let ((k (modulo n 16)))
          (loop (+ i 1) (floor (/ n 16))
                (string-append (substring "0123456789ABCDEF" k (+ k 1)) s))))))

; n's low d bits as 1s and 0s, the most significant first
(define (cp-bin n d)
  (let loop ((i 0) (n n) (s ""))
    (if (= i d) s
        (loop (+ i 1) (floor (/ n 2))
              (string-append (if (= 1 (modulo n 2)) "1" "0") s)))))

; how many of n's low d bits are 1
(define (cp-ones n d)
  (let loop ((i 0) (n n) (c 0))
    (if (= i d) c (loop (+ i 1) (floor (/ n 2)) (+ c (modulo n 2))))))

; The book's program that adds 10 + 9 + ... + 1, as the CPU runs it.
; Each instruction: its text, then op, and its three fields:
; addi rd rs1 imm; add rd rs1 rs2; bne rs1 rs2 offset (bytes).
(define cp-prog
  '(("addi x5, x0, 10" addi 5 0 10)
    ("addi x6, x0, 0" addi 6 0 0)
    ("add  x6, x6, x5" add 6 6 5)
    ("addi x5, x5, -1" addi 5 5 -1)
    ("bne  x5, x0, loop" bne 5 0 -8)))

; A state is (pc x5 x6); a register not yet written is the symbol ?.
(define (cp-get s r) (cond ((= r 5) (cadr s)) ((= r 6) (caddr s)) (else 0)))
(define (cp-put s r v pc)
  (list pc (if (= r 5) v (cadr s)) (if (= r 6) v (caddr s))))

; the instruction at the state's pc, as (op a b c)
(define (cp-ins s) (cdr (list-ref cp-prog (/ (car s) 4))))

; one instruction run: fetch at pc, decode, execute
(define (cp-step s)
  (let* ((in (cp-ins s)) (op (car in)) (a (cadr in)) (b (caddr in))
         (c (cadddr in)) (pc (car s)))
    (cond ((eq? op 'bne)
           (list (if (= (cp-get s a) (cp-get s b)) (+ pc 4) (+ pc c))
                 (cadr s) (caddr s)))
          ((eq? op 'add) (cp-put s a (+ (cp-get s b) (cp-get s c)) (+ pc 4)))
          (else (cp-put s a (+ (cp-get s b) c) (+ pc 4))))))

; the state after n instructions, from the start
(define (cp-run n)
  (let loop ((i 0) (s '(0 ? ?)))
    (if (= i n) s (loop (+ i 1) (cp-step s)))))

; a register's value as words: a number, or ? before it is written
(define (cp-val v) (if (number? v) (fmt v 0) "?"))
