# Adding

When we put two groups together we **add**: $2 + 3 = 5$.

$$ 2 + 3 = 5 $$

| first | second | together |
|---|---|---|
| 2 | 3 | 5 |

> Adding means putting together.

```check
(choice
  (ask "What is" (math "4 + 1") "?")
  (right "5" (true "4 + 1 = 5"))
  (wrong "6" (false "4 + 1 = 6") "Count on from 4: just one more.")
  (wrong "3" "Adding makes more, not less."))
```
