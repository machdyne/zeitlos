# Writing a file

```lesson
(objective "Save text to a file with fs_write_file, check that every byte was written, and read a number back.")
(minutes 20)
```

```c
int fs_write_file(char *path, char *buf, int len);
```

writes len bytes, starting at buf, to the file at path. If the file
is there it is **replaced**; if not, it is made. The folder must
already exist.

It gives back the number of bytes it wrote: len when all is well.
Anything else, such as -1, means the file was not written as asked.

```sources
(source "sw/apps/zcc/libz/syscall.c, fs_write_file(): the bytes written, or -1")
```

```check
(choice
  (ask "/home/c/log.txt holds 300 bytes. A program writes 20 bytes to it with fs_write_file. How big is the file now?")
  (right "20 bytes: the old contents are replaced")
  (wrong "320 bytes: the new bytes are added at the end"
    "fs_write_file replaces the whole file. To add to a file, read it, add, and write the lot.")
  (wrong "300 bytes: the first 20 are overwritten"
    "fs_write_file does not write into the old file; it replaces it with exactly the bytes given.")
  (hint "Does fs_write_file keep anything that was in the file before?"))
```

---

## Saving a best score

```c
int printf(const char *format, ...);
int snprintf(char *buf, unsigned size,
    const char *format, ...);
unsigned strlen(const char *s);
int fs_write_file(char *path, char *buf, int len);

int main(void)
{
    char buf[16];
    int len;

    snprintf(buf, sizeof buf, "%d\n", 1250);
    len = strlen(buf);
    if (fs_write_file("/home/c/best.txt", buf, len) != len) {
        printf("could not save\r\n");
        return 1;
    }
    printf("saved\r\n");
    return 0;
}
```

The file is ordinary text: you can open /home/c/best.txt in `text`. A
text file ends its lines with `\n` alone; `\r\n` is for the console.

```check
(number
  (ask "How many bytes long is /home/c/best.txt after this program?")
  (answer 5) (from "4 + 1")
  (wrong-if 4 "The four digits and the '\\n' after them: 5 characters, one byte each.")
  (wrong-if 6 "strlen does not count the 0 byte at the end of the string, so it is not written.")
  (hint "Count the characters snprintf put in buf: the digits of 1250, and \\n."))
```

---

## Reading the number back

libz's `atoi` turns the digits at the start of a string into an int:

```c
int atoi(const char *s);

char *t = fs_mallocfile("/home/c/best.txt");
int best = 0;
if (t != 0) {
    best = atoi(t);
    free(t);
}
```

If there is no file yet, best stays 0: a missing file is the normal
start of a game that has never been played.

```sources
(source "docs/libz.md, What is in it, and what is not: atoi")
```

```check
(number
  (ask "What does atoi(\"1250\\n\") give back?")
  (answer 1250)
  (wrong-if 0 "atoi reads the digits at the start and stops at the first character that is not one. 1250 comes first.")
  (hint "atoi reads digits until something that is not a digit."))
```

---

## Practice

```check
(choice
  (ask "A program saves a list of names by calling fs_write_file once for each name, on the same file. What is in the file at the end?")
  (right "Only the last name")
  (wrong "Every name, one after another"
    "Each call replaces the file. Build the whole list in one array, then write it once.")
  (wrong "Nothing: a file can be written only once"
    "A file can be written as often as you like. Each write replaces what was there.")
  (hint "What does each fs_write_file do to what the last one wrote?"))
```

```check
(choice
  (ask "fs_write_file(\"/home/games/best.txt\", buf, len) gives back -1. There is no folder /home/games. What is wrong?")
  (right "fs_write_file makes the file, but not the folder it goes in")
  (wrong "best.txt is a name a program may not use"
    "Any name will do. The folder the file goes in must already exist.")
  (wrong "-1 means it worked"
    "fs_write_file gives back the number of bytes written. -1 is a failure.")
  (hint "Which part of the path does not exist yet?"))
```

So: fs_write_file replaces a file with exactly the bytes you give it,
and its answer is the count of bytes written, to be checked.
