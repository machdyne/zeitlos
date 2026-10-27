# Scripts

```lesson
(objective "Try Scheme in slide's message box, and write a deck's script that keeps a count.")
(minutes 25)
(keywords "Scheme" "deck's script (in slide)" "message box (in slide)")
```

Plain actions go to cards, show things and say things. For more,
counting, choosing, keeping a score, a deck can use **Scheme**, a
programming language: a way of writing instructions for the computer.
It is the same Scheme as the `repl` app's.

Scheme is written in brackets: the first word says what to do, and the
rest is what to do it with. `(go 3)` goes to card 3; `(+ 2 3)` adds.

The quickest way to try it is the **message box**: on the
command line, a line that starts with `(` is Scheme, run at once, and
its answer comes back in the status line. `/(+ 2 3)` shows 5.

```sources
(source "Scripting slide (docs/slide_scripting.md), Where scripts live")
```

```check
(number
  (ask "You type /(* 6 7) and press Enter. What does the status line show? (* multiplies.)")
  (answer 42) (from "6 * 7")
  (wrong-if 13 "That adds. In Scheme, * multiplies: 6 times 7.")
  (hint "The first word, *, says what to do with 6 and 7."))
```

---

## slide's words in Scheme

| Scheme | Does |
|---|---|
| (go 'next), (go 3), (go "results") | another card |
| (show "name"), (hide "name") | an object by its name |
| (set-text "name" value) | any value as an object's words |
| (get-text "name") | an object's words, such as a field's |
| (get-value "name") | a check box's yes or no, a slider's number |
| (speak "words") | says them aloud |

A plain action is kept as Scheme, too: `go next` is `(go 'next)`.

```check
(choice
  (ask "A field is named age. Which Scheme gives what was typed in it?")
  (right "(get-text \"age\")")
  (wrong "(set-text \"age\")" "set-text puts words in. get-text takes them out.")
  (wrong "(get-value \"age\")" "get-value is for check boxes, radios, sliders and lists. A field's words come from get-text.")
  (hint "Getting a field's words."))
```

---

## Choosing

An action can be Scheme, typed as it is after `/action`:

`(if (> (get-value "volume") 5) (speak "Loud") (speak "Quiet"))`

`if` takes a question, what to do if it is true, and what to do if not.
`(> a b)` asks whether a is greater than b.

```check
(choice
  (ask "The slider volume is at 3. Its button's action is the one above. What is said?")
  (right "Quiet")
  (wrong "Loud" "3 is not greater than 5, so the second choice, Quiet, is said.")
  (wrong "Nothing" "if always does one of the two. The question is false, so it is the second.")
  (hint "Is 3 greater than 5?"))
```

---

## The deck's script

The **deck's script** is Scheme that runs when the deck starts
being used: with the browse tool, in a show, or as an app. It is where
you keep things for the whole deck.

`/script deck` opens it in `text`. Each time you save it there
(Ctrl+S), slide takes it, once its brackets all close, and says so in
the status line.

`(when-clicked "name" ...)` in the deck's script runs what follows when
the control of that name is clicked, on any card.

```example
(example
  (problem "Counting clicks: a button plus and a text box shown that counts the clicks.")
  (step "Make a button with the words +1 and name it plus. Make a text box with 0 in it, and name it shown.")
  (step "Type /script deck. In text, write: (define count 0), which makes a name, count, that holds 0.")
  (step "Below it: (when-clicked \"plus\" (set! count (+ count 1)) (set-text \"shown\" count))")
  (step "Save with Ctrl+S, go back to slide, and press F7.")
  (answer "Each click on +1 adds one to count and shows it."))
```

```check
(choice
  (ask "In the example, what does (set! count (+ count 1)) do?")
  (right "It makes count one more than it was")
  (wrong "It sets count to 1" "It adds 1 to what count was: (+ count 1).")
  (wrong "It shows count on the card" "set-text shows it. set! only changes the number kept.")
  (hint "Read the inside first: (+ count 1)."))
```

---

## When something goes wrong

A mistake is said in the status line, with where it came from, such as
`unbound symbol: cuont` for a misspelt name. The script stops; the
deck goes on. **Esc** stops a script that runs too long.

`(after 1000 ...)` runs what follows once, a second later, and `(every
1000 ...)` runs it every second; the times are in milliseconds.

```sources
(source "Scripting slide (docs/slide_scripting.md), Short forms; When something goes wrong")
```

```check
(choice
  (ask "Your count of clicks does nothing, and the status line says unbound symbol: cont. What is wrong?")
  (right "A name is misspelt: cont where count was meant")
  (wrong "The button is broken" "The button ran the script; the script used a name that was never defined.")
  (wrong "Scheme cannot count" "It can: the name has a letter missing.")
  (hint "Compare cont with the name in define."))
```

So: Scheme in brackets does more than plain actions; `/(...)` tries it
at once; and the deck's script, with `when-clicked`, makes a deck an
app of its own.
