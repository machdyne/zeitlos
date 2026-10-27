; Inside Zeitlos: helpers for the course's figures. Every name starts
; with zi- so nothing clashes with the library or another course.

; one hex digit, 0 to 15
(define (zi-hexd d) (substring "0123456789ABCDEF" d (+ d 1)))

; n as `digits` hex digits, most significant first
(define (zi-hex n digits)
  (let loop ((i (- digits 1)) (acc ""))
    (if (< i 0) acc
        (loop (- i 1)
              (string-append acc (zi-hexd (modulo (floor (/ n (expt 16 i))) 16)))))))

; a 32-bit address written the way the docs write it: 0x4000_0000
(define (zi-addr n)
  (string-append "0x" (zi-hex (floor (/ n 65536)) 4) "_" (zi-hex (modulo n 65536) 4)))

; a labelled box; when `on`, filled with ink and its label in paper
(define (zi-box x y w h label on)
  (if on (rect x y w h 'ink))
  (frame x y w h 'ink 1)
  (text (+ x 6) (+ y (/ h 2) 5) label 'small (if on 'paper 'ink)))

; the 16 regions of the address map, by the address's top hex digit
(define zi-regions
  '("boot RAM (BIOS)" "flash" "VRAM (screen)" "glyph memory"
    "main memory" "more main memory" "Ethernet" "system registers"
    "app window" "MTU and MPU" "line rasterizer" "SD card"
    "USB" "blitter" "GPIO pins" "serial (UART)"))

; the map as two columns of eight boxes, region `sel` filled
(define (zi-address-map sel)
  (for-range 0 16
    (lambda (k)
      (let ((x (if (< k 8) 8 164))
            (y (+ 8 (* 24 (modulo k 8)))))
        (zi-box x y 148 21
                (string-append "0x" (zi-hexd k) " " (list-ref zi-regions k))
                (= k sel))))))

; a 6 x 6 grid of pixels at (x, y), cells c units wide; (f i j) says
; whether pixel column i, line j is set
(define (zi-pixels x y c f)
  (for-range 0 6
    (lambda (j)
      (for-range 0 6
        (lambda (i)
          (if (f i j) (rect (+ x (* i c)) (+ y (* j c)) c c 'ink))))))
  (frame x y (* 6 c) (* 6 c) 'ink 1))

; the two inputs of the raster-op figure
(define (zi-dst i j) (< i 3))
(define (zi-src i j) (and (> i 0) (< i 5) (> j 0) (< j 5)))

; Bresenham's line from (x0, y0) to (x1, y1), calling (plot x y) for
; each pixel, as the line rasterizer does; returns how many it plotted
(define (zi-bresenham x0 y0 x1 y1 plot)
  (let* ((dx (abs (- x1 x0))) (dy (- (abs (- y1 y0))))
         (sx (if (< x0 x1) 1 -1)) (sy (if (< y0 y1) 1 -1)))
    (let loop ((x x0) (y y0) (err (+ dx dy)) (count 1))
      (plot x y)
      (if (and (= x x1) (= y y1)) count
          (let* ((e2 (* 2 err))
                 (nx (if (>= e2 dy) (+ x sx) x))
                 (ny (if (<= e2 dx) (+ y sy) y))
                 (ne (+ err (if (>= e2 dy) dy 0) (if (<= e2 dx) dx 0))))
            (loop nx ny ne (+ count 1)))))))
