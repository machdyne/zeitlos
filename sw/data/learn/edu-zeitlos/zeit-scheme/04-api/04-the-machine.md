# The time and other apps

```lesson
(objective "Read the date and time, wait, and start other apps from Scheme.")
(minutes 15)
(keywords "process (a running program)" "pid (process number)")
```

`current-date` gives the date and time as a list of numbers:

```
> (current-date)
(2026 10 26 14 5 9 1 298)
```

In order: year, month, day, hour, minute, second, the day of the week
(0 is Sunday) and the day of the year (0 is January 1). The time is
UTC, the time at Greenwich, which may not be your own. If the computer's clock was never set, the
answer is `#f`.

Since it is a list, `list-ref` takes out one part: `(list-ref
(current-date) 3)` is the hour.

```sources
(source "Zeitlos docs: zapi.md, Time and memory")
```

```check
(number
  (ask "`(current-date)` gives `(2026 3 9 18 30 0 1 67)`. Which month is it, as a number?")
  (answer 3)
  (wrong-if 9 "9 is the day. The parts go year, month, day: the month is second.")
  (hint "The second number in the list."))
```

---

## A clock you can read

The date's parts are numbers. To show them, turn them into strings:

```
(define (hh-mm d)
  (string-append (number->string (list-ref d 3))
                 ":"
                 (number->string (list-ref d 4))))
```

`(hh-mm (current-date))` might give `"14:5"`. Minutes under 10 come out
with one digit; an `if` that puts `"0"` in front of them would give
`"14:05"`.

```check
(choice
  (ask "Why `(list-ref d 4)` for the minutes?")
  (right "Counting from 0, the minutes are item 4: year 0, month 1, day 2, hour 3")
  (wrong "Because there are 4 parts before the time"
    "There are three parts before the hour, and the hour is one more. list-ref counts from 0.")
  (wrong "The minute is always the fourth number"
    "The fourth number, counting from 1, is the hour. list-ref counts from 0, so 4 is the fifth: the minutes.")
  (hint "list-ref counts from 0."))
```

---

## Waiting

`(delay-ms 500)` waits half a second, then answers `#t`. While it
waits, repl does nothing else: no terminal and no window of repl's can
answer. Keep waits short.

`(uptime)` counts the ticks of the system's clock since the computer
started, about 732 a second.

```check
(number
  (ask "`(uptime)` gives 7320. About how many seconds ago did the computer start?")
  (answer 10) (from "7320 / 732")
  (wrong-if 7320 "uptime counts ticks, not seconds: about 732 of them a second.")
  (hint "Divide by 732."))
```

---

## Other apps

Every running app is a **process**, with a number of its own, its
**pid**. `run` starts an app by its name and answers with the new
process's pid, or `#f` if it could not:

```
> (run "clock")
12
> (run "view" "/media/images/squirrel.jpg")
13
```

The second argument is a file for the app to open, as `files` would
open it. `(ps)` lists the processes, one list each, starting with its
pid; `(kill pid)` ends one at once, without asking.

```sources
(source "Zeitlos docs: zapi.md, Processes and messages")
```

```check
(choice
  (ask "What does `(map car (ps))` give?")
  (right "The pid of every process")
  (wrong "The name of every app"
    "Each process's list starts with its pid, a number. car of each is that pid.")
  (wrong "The first process only"
    "map calls car on every process's list, so it gives one answer for each.")
  (hint "Each process's list starts with its pid."))
```

---

## Try it

```try
(try (app "repl")
  (task "In repl, type (run \"clock\"). A clock opens. Then type (ps) to see the processes. Come back here.")
  (check (choice (ask "What did (run \"clock\") answer?")
    (right "A number: the new process's pid")
    (wrong "#t" "run answers with the pid of the app it started, a number.")
    (wrong "The time" "run starts the app; the app shows the time. run's answer is the pid.")
    (hint "run gives the new process's number."))))
```

```check
(choice
  (ask "You type `(delay-ms 5000)`. What happens in repl for five seconds?")
  (right "Nothing: repl waits and answers nothing until it is done")
  (wrong "repl goes on working, and #t appears after five seconds"
    "delay-ms makes all of repl wait. Its windows and terminals answer only when it is done.")
  (wrong "The computer stops"
    "Only repl waits. Other apps go on as before.")
  (hint "Read what delay-ms does to repl while it waits."))
```

So: `current-date` and `uptime` tell the time, and `run` and `ps`
start and list the apps, each a process with a pid.
