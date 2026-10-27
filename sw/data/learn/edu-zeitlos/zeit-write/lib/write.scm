; Writing: a window, lines of text, and a paragraph wrapped to a width,
; as the figures of zeit-write draw them. Text is 'small (7 x 13 units
; a character), so a line of n characters is 7n units wide.

(define wr-cw 7)	; units per character, 'small
(define wr-lh 14)	; units per line

; A window: blank inside, a frame, and a dark title bar with the title.
(define (wr-window x y w h title)
  (rect x y w h 'paper)
  (frame x y w h 'ink 1)
  (rect x y w 16 'ink)
  (text (+ x 6) (+ y 12) title 'paper 'small))

; Lines of text, one under another, the first with its baseline at y.
(define (wr-lines x y lines)
  (let loop ((l lines) (y y))
    (if (pair? l)
        (begin (text x y (car l) 'small)
               (loop (cdr l) (+ y wr-lh))))))

; A list of words wrapped into lines of at most n characters: as many
; whole words on a line as fit, a space between them.
(define (wr-wrap words n)
  (let loop ((ws words) (line "") (out '()))
    (cond ((null? ws)
           (reverse (if (string=? line "") out (cons line out))))
          ((string=? line "")
           (loop (cdr ws) (car ws) out))
          ((<= (+ (string-length line) 1 (string-length (car ws))) n)
           (loop (cdr ws) (string-append line " " (car ws)) out))
          (else (loop ws "" (cons line out))))))

; Words drawn light on dark, as read draws `code` and a selection.
(define (wr-inverse x y s)
  (rect (- x 1) (- y 10) (+ 2 (* wr-cw (string-length s))) 13 'ink)
  (text x y s 'paper 'small))

; Two views of the same document: what is typed in text, and what read
; shows. A choice control; returns #t for read's view.
(define (wr-view)
  (= 1 (choice 'view '("In text" "In read") "View")))
