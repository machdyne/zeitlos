# Pointers

```lesson
(objective "Use & and * to take an address and reach the variable at it, and let a function change its caller's variable.")
(minutes 25)
(keywords "address (in memory)" "pointer (in C)")
```

Every byte of memory has a number, its **address**, as every house in
a street has one. A variable occupies some bytes, and its address is
the address of the first: an int takes 4 bytes, so two ints side by
side have addresses 4 apart.

In C, `&x` is the address of the variable x. `printf("%p", &x)` would
print it.

```sources
(source "Timeless Computing (docs/tc.md), C: Pointers")
```

```check
(number
  (ask "An int starts at address 1000. At what address does the next byte after it start?")
  (answer 1004) (from "1000 + 4")
  (wrong-if 1001 "1001 is the int's own second byte. An int takes 4 bytes: 1000 to 1003.")
  (hint "An int is 4 bytes long."))
```

---

## A pointer

A **pointer** is a variable that holds an address:

```c
int x = 5;
int *p = &x;
*p = 7;
```

`int *p` makes p a pointer to an int. In a statement, `*p` means "the
int at the address in p". Storing through p changes x, so x is now 7.

Toggle to point p at x or at y, and see what `*p` is.

```figure
(define at-y (toggle 'y "p = &y" #f))
(text 60 40 "x" 'centre 'body)
(text 140 40 "y" 'centre 'body)
(text 240 40 "p" 'centre 'body)
(zc-box 30 50 60 30 "5")
(zc-box 110 50 60 30 "9")
(zc-box 210 50 60 30 (if at-y "1004" "1000"))
(text 60 98 "1000" 'centre 'small)
(text 140 98 "1004" 'centre 'small)
(text 240 98 "1008" 'centre 'small)
(path (list 240 104 240 118 (if at-y 140 60) 118) 'ink 1.5)
(arrow (if at-y 140 60) 118 (if at-y 140 60) 104 'ink 1.5)
(text 160 170 (string-append "*p is " (if at-y "9" "5")) 'centre 'big)
(caption "x is 5, y is 9; small numbers: addresses")
```

```check
(number
  (ask "int a = 3; int \\*q = &a; \\*q = \\*q + 10; What is a now?")
  (answer 13) (from "3 + 10")
  (wrong-if 3 "q points to a, so \\*q is a itself. Storing into \\*q stores into a.")
  (hint "\\*q is another name for a, as long as q holds &a."))
```

---

## Changing the caller's variable

A function gets copies of its parameters, so `twice(x)` could not
change x. Pass x's address instead, and the function can reach x
itself:

```c
void twice(int *n)
{
    *n = *n * 2;
}
```

The call is `twice(&x);`. n holds x's address; `*n` is x.

This is how a C function gives back more than one result: the caller
passes the addresses of variables for the answers.

```check
(choice
  (ask "int x = 4; Which call leaves x at 8, with twice(int \\*n) above?")
  (right "twice(&x);")
  (wrong "twice(x);"
    "twice wants an address. x is the value 4; &x is where x is.")
  (wrong "twice(\\*x);"
    "x is an int, not a pointer, so \\*x means nothing here. The address of x is &x.")
  (hint "Which sign gives the address of a variable?"))
```

---

## Arrays are addresses

In most expressions, an array's name stands for the address of its
first element. So a function given an array is given its address, and
works on the caller's array, not a copy. `char s[]` and `char *s` mean
the same as a parameter.

Adding to a pointer moves it by whole elements: `s + 2` is the address
of `s[2]`, and `s[i]` is the same as `*(s + i)`.

Now printf's declaration reads fully: `const char *format` is a pointer
to chars that printf will not change (`const`).

```sources
(source "Timeless Computing (docs/tc.md), C: Arrays and Strings")
```

```check
(choice
  (ask "int a[4] = { 10, 20, 30, 40 }; int \\*q = a; What is \\*(q + 2)?")
  (right "30")
  (wrong "12"
    "Adding 2 to a pointer moves it 2 ints along, not 2 bytes: to a[2], which holds 30.")
  (wrong "20"
    "q + 0 is a[0], so q + 2 is a[2], the third element.")
  (hint "\\*(q + 2) is the same as q[2]."))
```

---

## Pointing nowhere

A pointer that holds 0 points to nothing. Functions that hand back a
pointer, such as libz's `malloc`, which finds memory for a program,
and `fs_mallocfile`, which reads a file into memory, return 0 when
they fail. Storing through such a pointer, or through one that was
never given an address, writes somewhere the program does not own.

So check a pointer before you use it:

```c
if (p == 0) {
    printf("nothing there\r\n");
    return 1;
}
```

```check
(choice
  (ask "int \\*p; \\*p = 5; with nothing stored in p first. What is wrong?")
  (right "p holds no address of ours, so 5 is written to an unknown place")
  (wrong "Nothing: p now points to 5"
    "\\*p = 5 stores 5 where p points. p was never given an address, so where that is, nobody knows.")
  (wrong "zcc stops with an error"
    "zcc compiles it: C trusts the program. The mistake shows only when it runs.")
  (hint "Where does \\*p = 5 put the 5?"))
```

So: & gives an address, * reaches what is at one, and passing an
address lets a function change its caller's variables.
