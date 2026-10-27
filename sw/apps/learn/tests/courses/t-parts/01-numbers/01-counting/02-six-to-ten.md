# Six to ten

Counting goes on: after five come six, seven, eight, nine and ten.

1. Start at one.
2. Say each number once.
   Point as you go.
3. Stop at the last thing.

---

```check
(number
  (ask "What number comes after 7?")
  (answer 8)
  (wrong-if 6 "6 comes before 7. Which comes after?")
  (hint "Count up: five, six, seven, ..."))
```
