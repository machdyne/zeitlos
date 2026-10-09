; todo.scm -- a to-do list, as a window: an example of a Zeitlos app
; written in Scheme. docs/repl.md walks through it.
;
;   > (load "/data/repl/examples/todo.scm")
;
; Type a task and press Enter (or Add). Click a task to tick it off;
; Clear removes the ticked ones.

(define w (win-create "To do" 200 160))
(define entry (field w 4 4 140))
(define add (button w 148 4 46 16 "Add"))
(define clear (button w 148 140 46 16 "Clear"))

; the list: each task is (text . done)
(define tasks '())

(define (draw-tasks)
  (box w 0 24 199 137 0)                     ; blank the list area
  (define (draw l y)
    (if (pair? l)
        (begin
          (text w 4 y (if (cdr (car l)) "[x]" "[ ]") 1)
          (text w 28 y (car (car l)) 1)
          (draw (cdr l) (+ y 12)))))
  (draw tasks 26))

(define (add-task)
  (let ((s (field-text w entry)))
    (if (> (string-length s) 0)
        (begin
          (set! tasks (append tasks (list (cons s #f))))
          (field-set! w entry "")
          (draw-tasks)))))

(define (tick l n)                           ; flip task n
  (cond ((null? l) '())
        ((= n 0) (cons (cons (car (car l)) (not (cdr (car l)))) (cdr l)))
        (else (cons (car l) (tick (cdr l) (- n 1))))))

(define (undone l)                           ; the tasks not ticked
  (cond ((null? l) '())
        ((cdr (car l)) (undone (cdr l)))
        (else (cons (car l) (undone (cdr l))))))

(win-on w
  (lambda (e)
    (case (car e)
      ((redraw) (draw-tasks))
      ((enter) (add-task))
      ((button)
       (if (= (cadr e) add) (add-task))
       (if (= (cadr e) clear)
           (begin (set! tasks (undone tasks)) (draw-tasks))))
      ((click)
       (let ((y (caddr e)))
         (if (>= y 26)
             (begin
               (set! tasks (tick tasks (quotient (- y 26) 12)))
               (draw-tasks)))))
      ((close) (print "to do: closed")))))
