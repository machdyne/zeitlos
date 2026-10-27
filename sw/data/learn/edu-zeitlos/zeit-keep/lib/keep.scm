; Keeping your work: figures.

; The backups figure: three backups (columns) of three files (rows). A
; filled box is a copy stored in that backup's folder. The first backup
; holds everything; later ones only what had changed.
(define keep-days '("19 Oct" "20 Oct" "21 Oct"))
(define keep-files '("book.txt" "notes.txt" "budget.zss"))
(define keep-copies '((1 1 1) (1 0 0) (1 1 0)))

(define (keep-col i) (+ 140 (* i 66)))
(define (keep-row j) (+ 78 (* j 40)))

; the newest backup (column) holding a copy of file j
(define (keep-newest j)
  (let loop ((i 2))
    (if (= 1 (list-ref (list-ref keep-copies j) i)) i (loop (- i 1)))))

; draws the grid; file j (or -1 for none) is picked out
(define (keep-backups j)
  (for-range 0 3
    (lambda (i)
      (text (keep-col i) 40 (list-ref keep-days i) 'centre 'small 'bold)
      (frame (- (keep-col i) 30) 50 60 132 'grey 1)))
  (for-range 0 3
    (lambda (r)
      (text 16 (+ (keep-row r) 4) (list-ref keep-files r) 'small (if (= r j) 'bold 'ink))
      (for-range 0 3
        (lambda (i)
          (if (= 1 (list-ref (list-ref keep-copies r) i))
              (rect (- (keep-col i) 14) (- (keep-row r) 9) 28 18 'warm))))))
  (if (>= j 0)
      (frame (- (keep-col (keep-newest j)) 22) (- (keep-row j) 15) 44 30 'ink 2.5)))

; The three copies figure: the card, a backup stick at home, a second
; copy kept elsewhere. e is what goes wrong (0 the card fails, 1 a file
; is deleted by mistake, 2 a fire at home, 3 the backup stick fails);
; mirror: the stick is a mirror (kept the same as the card), not dated
; backups.
(define keep-places '("The card" "Backup stick" "Second copy"))
(define keep-where '("in the computer" "at home" "somewhere else"))

(define (keep-lost? e mirror p)
  (cond ((= e 0) (= p 0))
        ((= e 1) (or (= p 0) (and mirror (= p 1))))
        ((= e 2) (< p 2))
        (else (= p 1))))

(define (keep-copies-fig e mirror)
  (let loop ((p 0) (left 0))
    (if (< p 3)
        (let ((x (+ 12 (* p 102))) (lost (keep-lost? e mirror p)))
          (frame x 40 92 140 'ink (if lost 1 2.5))
          (text (+ x 46) 62 (list-ref keep-places p) 'centre 'small 'bold)
          (text (+ x 46) 78 (list-ref keep-where p) 'centre 'tiny)
          (if lost
              (begin (line (+ x 26) 100 (+ x 66) 136 'ink 2)
                     (line (+ x 66) 100 (+ x 26) 136 'ink 2)
                     (text (+ x 46) 160 "lost" 'centre 'small))
              (text (+ x 46) 122 "has the file" 'centre 'small))
          (loop (+ p 1) (if lost left (+ left 1))))
        (text 160 205 (string-append "Copies left: " (fmt left 0) " of 3") 'centre 'body 'bold))))
