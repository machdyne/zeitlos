; Getting started: the desktop as the figures draw it. A sketch, not to
; scale: the real screen is 640 x 480, a titlebar 11 pixels high.

; a window: its frame, a titlebar with the title on the left and the
; close box on the right (filled when it has the focus), and, for a
; window that can be resized, the grip's ticks in the lower-right corner
(define (zs-win x y w h title focused grip)
  (let ((r (+ x w)) (b (+ y h)) (fg (if focused 'paper 'ink)))
    (rect x y w h 'paper)
    (if focused (rect x y w 14 'ink) (line x (+ y 14) r (+ y 14) 'ink 1))
    (frame x y w h 'ink 1)
    (text (+ x 5) (+ y 11) title 'small fg)
    (frame (- r 12) (+ y 3) 8 8 fg 1)
    (if grip
        (begin
          (line (- r 4) (- b 1) (- r 1) (- b 4) 'ink 1)
          (line (- r 8) (- b 1) (- r 1) (- b 8) 'ink 1)))))

; the dock: a frame along the bottom of the screen holding n icons, each
; a small box; the first `dots` of them with a running dot under them
(define (zs-dock x y n dots)
  (frame x y (+ 4 (* n 18)) 24 'ink 1)
  (for-range 0 n
    (lambda (i)
      (let ((ix (+ x 3 (* i 18))))
        (frame ix (+ y 3) 16 16 'ink 1)
        (if (< i dots) (rect (+ ix 7) (+ y 20) 2 2 'ink))))))

; the screen's edge
(define (zs-screen) (frame 0 0 320 240 'ink 1.5))

; a dock of `apps` app icons and then `groups` group icons (a box with a
; thick bar across its top); `dots` is a list of running-dot counts, one
; for each icon from the first (shorter is fine); and, if given, how many
; icons wide the frame is (an open group keeps the row's width)
(define (zs-dock-row x y apps groups dots . wide)
  (let ((n (+ apps groups)))
    (frame x y (+ 4 (* (if (pair? wide) (car wide) n) 18)) 24 'ink 1)
    (for-range 0 n
      (lambda (i)
        (let ((ix (+ x 3 (* i 18)))
              (d (if (< i (length dots)) (list-ref dots i) 0)))
          (frame ix (+ y 3) 16 16 'ink 1)
          (if (>= i apps) (rect ix (+ y 3) 16 5 'ink))
          (for-range 0 d
            (lambda (k) (rect (+ ix 8 (* 4 k) (* -2 d)) (+ y 20) 2 2 'ink))))))))
