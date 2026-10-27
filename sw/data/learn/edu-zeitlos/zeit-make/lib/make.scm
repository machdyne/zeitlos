; Making things: the figures' own procedures. Names start mk-.

; a labelled box: an outline, its words in the middle
(define (mk-box x y w h s)
  (frame x y w h 'ink 1)
  (text (+ x (/ w 2)) (+ y (/ h 2) 4) s 'centre 'small))

; slide's window, as a plan: the tools down the left, the card, the
; bar under it and the status line. With `sel`, the letter of that
; tool (an index into mk-slide-tools) is shown chosen.
(define mk-slide-tools '("b" "v" "t" "r" "u" "o" "l" "a" "p" "f" "i"))
(define (mk-slide-window sel label)
  (frame 10 8 300 206 'ink 1.5)
  (rect 10 8 300 16 'ink)
  (text 18 20 "untitled deck" 'paper 'small)
  (line 34 24 34 182 'ink 1)
  (let loop ((l mk-slide-tools) (i 0))
    (if (pair? l)
        (begin
          (if (= i sel) (rect 12 (+ 26 (* i 14)) 20 14 'ink))
          (text 22 (+ 37 (* i 14)) (car l) 'centre 'small (if (= i sel) 'paper 'ink))
          (loop (cdr l) (+ i 1)))))
  (rect 52 36 244 136 'paper)
  (frame 52 36 244 136 'ink 1)
  (text 174 108 label 'centre 'body)
  (line 10 182 310 182 'ink 1)
  (text 40 196 "fill   line   font   B I U" 'small)
  (text 18 209 "Card 1/1  card 1   / for commands" 'tiny))

; a picture given as rows of characters, "#" for ink, drawn with each
; pixel z units square from (x, y); a grid when z is 6 or more
(define (mk-pixels rows x y z)
  (let loop ((r rows) (j 0))
    (if (pair? r)
        (begin
          (let ((s (car r)))
            (for-range 0 (string-length s)
              (lambda (i)
                (if (char=? (string-ref s i) #\#)
                    (rect (+ x (* i z)) (+ y (* j z)) z z 'ink)))))
          (loop (cdr r) (+ j 1)))))
  (if (>= z 6)
      (let ((w (string-length (car rows))) (h (length rows)))
        (for-range 0 (+ w 1)
          (lambda (i) (line (+ x (* i z)) y (+ x (* i z)) (+ y (* h z)) 'grey 0.5)))
        (for-range 0 (+ h 1)
          (lambda (j) (line x (+ y (* j z)) (+ x (* w z)) (+ y (* j z)) 'grey 0.5))))))

; a card of a deck, small, at (x, y): w x h, with a band across its top
; when band is true, and words in the middle
(define (mk-card x y w h band s)
  (rect x y w h 'paper)
  (frame x y w h 'ink 1)
  (if band (rect (+ x 1) (+ y 1) (- w 2) 12 'grey))
  (text (+ x (/ w 2)) (+ y (/ h 2) 8) s 'centre 'small))
