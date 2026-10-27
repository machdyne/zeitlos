# Arrays and strings

```lesson
(objective "Use arrays with a loop, and treat a string as an array of chars ending in a zero byte.")
(minutes 20)
(keywords "array (in C)" "element (of an array)" "index (of an array)" "buffer overflow")
```

An **array** is a row of variables of one type, under one name:

```c
int marks[5] = { 70, 85, 60, 90, 75 };
```

Each one, an **element** of the array, is reached by its **index**,
its place in the row, counted from 0: `marks[0]` is 70 and `marks[4]`
is 75. An index can be any whole-number expression, such as a loop's
variable: `marks[i]`.

```check
(number
  (ask "An array is declared int a[10];. What is the index of its last element?")
  (answer 9) (from "10 - 1")
  (wrong-if 10 "Indexes start at 0, so ten elements are a[0] to a[9]. a[10] is past the end.")
  (hint "The first is a[0]. Count ten from there."))
```

---

## A loop over an array

```c
int total = 0;
for (int i = 0; i < 5; i++)
    total += marks[i];
printf("average %d\r\n", total / 5);
```

The loop runs i from 0 while `i < 5`: exactly the five indexes, 0 to 4.
That pattern, starting at 0 and stopping before the size, fits every
array.

```check
(number
  (ask "With the marks above, what does the program print after average?")
  (answer 76) (from "(70 + 85 + 60 + 90 + 75) / 5")
  (wrong-if 380 "380 is the total. The program divides it by 5.")
  (hint "Add the five marks, then divide by 5."))
```

---

## No checks

C does not check an index. `marks[5] = 0;` compiles, and when it runs
it writes into whatever memory comes after the array, perhaps another
variable. Nothing reports it, and the damage shows up later,
somewhere else.

Writing past the end of an array is a **buffer overflow**, one of the
most common causes of security holes. So every index must be one you
know is inside the array: `i < 5` in the loop, not `i <= 5`.

```sources
(source "Timeless Computing (docs/tc.md), C: Arrays and Strings")
```

```check
(choice
  (ask "for (int i = 0; i <= 5; i++) marks[i] = 0; with int marks[5]. What happens?")
  (right "It also writes 0 one place past the end of the array, into memory that is not marks")
  (wrong "zcc refuses to compile it"
    "zcc cannot know at compile time what i will be, and C does not ask. It compiles.")
  (wrong "The program stops with an error at marks[5]"
    "C does not check indexes while the program runs either. The write simply happens, past the end.")
  (hint "i <= 5 lets i reach 5. Is marks[5] inside a five-element array?"))
```

---

## Strings are arrays

A string is an array of chars, ending with a byte of 0, written
`'\0'`. That end marker is how a function finds where the string
stops.

```c
char name[] = "zeit";
```

makes an array of 5 chars: the four letters and the 0. Move the slider
to see each `name[i]` in turn.

```figure
(define i (slider 'i 0 4 1 "Index" 1))
(define cells '("z" "e" "i" "t" "\\0"))
(zc-cells 60 60 40 cells i)
(text 160 150 (string-append "name[" (fmt i 0) "] is '" (list-ref cells i) "'") 'centre 'body)
(caption "char name[] = \"zeit\";")
```

```check
(number
  (ask "How many bytes does char word[] = \"hello\"; take?")
  (answer 6) (from "5 + 1")
  (wrong-if 5 "Five letters, plus the 0 byte that ends every string.")
  (hint "Count the letters, then add the end marker."))
```

---

## Walking a string

`printf`'s `%s` prints a string. Your own code walks one until it
meets the 0:

```c
int length(char s[])
{
    int n = 0;
    while (s[n] != '\0')
        n++;
    return n;
}
```

A parameter `char s[]` is given the array itself, not a copy of it:
the next lesson shows why. libz has this function already, as `strlen`,
with `strcpy`, `strcmp` and others; you declare each one you use, as
you did printf.

```sources
(source "docs/libz.md, What is in it, and what is not")
```

```check
(number
  (ask "What does length(\"timeless\") return?")
  (answer 8)
  (wrong-if 9 "length counts the chars before the 0 byte; the 0 itself is not counted.")
  (hint "Count the letters of timeless."))
```

```check
(choice
  (ask "A string is copied into char box[4]. Which string fits safely?")
  (right "\"abc\"")
  (wrong "\"abcd\""
    "abcd is 4 letters plus the 0 byte: 5 bytes into 4. The 0 lands past the end of box.")
  (wrong "Any string: the array grows to fit"
    "A C array never grows. Copying in more than it holds is a buffer overflow.")
  (hint "A string needs one byte more than its letters."))
```

So: an array's indexes run from 0 to one less than its size, nobody
checks them for you, and a string is an array of chars ending in 0.
