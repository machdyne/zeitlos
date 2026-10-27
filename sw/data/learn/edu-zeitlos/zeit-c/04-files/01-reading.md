# Reading a file

```lesson
(objective "Read a whole file into memory with fs_mallocfile, check that it worked, and free the memory afterwards.")
(minutes 20)
(keywords "heap (of a program)" "free (memory)" "memory leak")
```

libz reads a whole file in one call:

```c
char *fs_mallocfile(char *path);
```

It finds memory for the file, reads the file into it, puts a 0 byte
after the last character, and gives back a pointer to the first. So
what comes back is a string: the whole file as text.

It gives back 0 if the file is missing, empty, or will not fit. Always
check before using the pointer.

```sources
(source "sw/apps/zcc/libz/syscall.c, fs_mallocfile()")
(source "sw/common/zfsapp.h, fs_mallocfile")
```

```check
(choice
  (ask "char \\*t = fs_mallocfile(\"/home/c/notes.txt\"); and there is no such file. What is t?")
  (right "0: nothing was read")
  (wrong "An empty string, \"\""
    "fs_mallocfile gives back 0, a pointer to nothing, when it cannot read the file. An empty string would still point somewhere.")
  (wrong "The program stops with an error"
    "Nothing stops. fs_mallocfile reports the failure by giving back 0, and the program must check for it.")
  (hint "What does a pointer function give back when it fails?"))
```

---

## Counting lines

```c
int printf(const char *format, ...);
char *fs_mallocfile(char *path);
void free(void *p);

int main(void)
{
    char *text = fs_mallocfile("/home/c/first.c");
    int lines = 0;
    int i;

    if (text == 0) {
        printf("cannot read it\r\n");
        return 1;
    }
    for (i = 0; text[i] != '\0'; i++)
        if (text[i] == '\n')
            lines++;
    printf("%d characters, %d lines\r\n", i, lines);
    free(text);
    return 0;
}
```

text is a pointer, but `text[i]` reaches its characters as an array's,
as the pointers lesson showed. Each line of a text file ends with the
character `'\n'`, so counting those counts the lines.

```check
(choice
  (ask "After the for loop, why is i the number of characters?")
  (right "The loop stops at the 0 byte after the last character, and i counted up from 0 to it")
  (wrong "i counts the lines"
    "lines counts the lines. i goes up by one for every character until the loop meets the 0 byte.")
  (wrong "i is always 0 after the loop"
    "i was declared before the loop, not in it, so it keeps the value it had when the loop stopped.")
  (hint "The loop stops when text[i] is the 0 byte. What is i then?"))
```

---

## The heap, and free

The memory fs_mallocfile finds comes from the program's **heap**:
memory a program asks for while it runs, through libz's `malloc`. It
stays the program's until it is given back with **free**, or the
program ends.

Free each block exactly once, after its last use. A block never freed
is a **memory leak**: a program that leaks a block each time round a
loop runs out of memory in the end. Using a block after freeing it, or
freeing it twice, damages the heap, and the program goes wrong later
somewhere else.

```sources
(source "Timeless Computing (docs/tc.md), C: Structures and Memory")
```

```check
(choice
  (ask "A program reads the same file with fs_mallocfile once a second, and never calls free. What happens?")
  (right "Each read takes new memory, until there is none and fs_mallocfile gives back 0")
  (wrong "Nothing: each read reuses the last block"
    "fs_mallocfile asks for a new block each time. The old ones stay taken until they are freed.")
  (wrong "libz frees the old blocks itself"
    "libz has no way to know you are finished with a block. Only free gives it back.")
  (hint "Where does each call's memory come from, and when does it go back?"))
```

---

## 16 KB

A program zcc makes gets **16 KB** for its heap and its local
variables together. So a file of more than about 15 KB cannot be read
whole: fs_mallocfile gives back 0. The next lessons write files, and
read big ones a piece at a time.

```sources
(source "docs/executables.md, The stack size: 16KB by default; sw/apps/zcc/emit.c writes flags 0")
(source "sw/apps/zcc/libz/mem.c: the heap grows up to the stack")
```

```check
(choice
  (ask "fs_mallocfile gives back 0 for a 30 KB file that is certainly there. Why?")
  (right "It does not fit in the program's 16 KB of heap and stack")
  (wrong "Files over 16 KB cannot be stored"
    "The card holds files far larger. The limit is the program's own memory, not the file's size on the card.")
  (wrong "The path must not start with /"
    "Full paths start with /. The trouble is size: 30 KB will not fit in 16 KB.")
  (hint "How much memory does a zcc program have for its heap?"))
```

```check
(choice
  (ask "Which test counts the letter e in text?")
  (right "if (text[i] == 'e') count++;")
  (wrong "if (text[i] == \"e\") count++;"
    "Double quotes make a string, which is an address. A single character is written in single quotes: 'e'.")
  (wrong "if (text == 'e') count++;"
    "text is the pointer to the whole file. One character is text[i].")
  (hint "One character, in single quotes, compared with one element."))
```

So: fs_mallocfile reads a whole small file as a string, gives back 0
when it cannot, and its memory goes back with free.
