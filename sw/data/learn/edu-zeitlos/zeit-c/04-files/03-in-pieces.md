# A file in pieces

```lesson
(objective "Read a file of any size a piece at a time through a handle, and close the handle when done.")
(minutes 20)
(keywords "handle (of an open file)" "buffer (in memory)")
```

A file too big for the program's memory is read a piece at a time.
First open it:

```c
int fs_open_read(const char *path);
```

gives back a **handle**: a number that stands for the open file in the
calls that follow. It gives back -1 if the file cannot be
opened, for example when there is no such file.

```sources
(source "sw/common/zfsapp.h, chunked file I/O")
(source "sw/apps/zcc/libz/syscall.c, fs_open_read(), fs_read_chunk(), fs_close_handle()")
```

```check
(choice
  (ask "int h = fs_open_read(\"/home/c/big.txt\"); Which test tells whether the file was opened?")
  (right "if (h < 0)  /\\* it was not \\*/")
  (wrong "if (h == 0)  /\\* it was not \\*/"
    "Failure is -1, not 0, so h == 0 never catches it. Test for a number below 0.")
  (wrong "if (h == '\\0')  /\\* it was not \\*/"
    "A handle is a number, not a character, and '\\0' is just 0. Failure is -1, so this never catches it.")
  (hint "What does fs_open_read give back when it fails?"))
```

---

## A loop of reads

```c
int printf(const char *format, ...);
int fs_open_read(const char *path);
int fs_read_chunk(int h, void *buf, int max);
int fs_close_handle(int h);

char buf[512];

int main(void)
{
    int h = fs_open_read("/home/c/big.txt");
    int lines = 0;
    int got;

    if (h < 0) {
        printf("cannot open it\r\n");
        return 1;
    }
    while ((got = fs_read_chunk(h, buf, sizeof buf)) > 0)
        for (int i = 0; i < got; i++)
            if (buf[i] == '\n')
                lines++;
    fs_close_handle(h);
    printf("%d lines\r\n", lines);
    return 0;
}
```

buf is a **buffer**: an array that holds data on its way somewhere.
Each fs_read_chunk fills it with up to 512 bytes, the next piece of
the file, and gives back how many: 0 at the end of the file, below 0
on a failure. `(got = ...) > 0` stores the count in got and then
tests it.

```check
(number
  (ask "big.txt is 2000 bytes. How many times is fs_read_chunk called, counting the last call that gives back 0?")
  (answer 5) (true "3 * 512 < 2000") (true "4 * 512 >= 2000")
  (wrong-if 4 "Four calls bring in all 2000 bytes (512, 512, 512, 464). The loop only stops when a fifth call gives back 0.")
  (wrong-if 3 "Three calls bring 1536 bytes; 464 are still to come, and then a call that gives back 0.")
  (hint "How many pieces of at most 512 make 2000? Then one more call finds the end."))
```

---

## Not a string

What fs_read_chunk puts in buf is just bytes: no 0 byte is added
after them. So the program goes by got, the count, not by looking for
a 0. `i < got` stops at the end of what this call read; the rest of
buf still holds the last piece's bytes.

```check
(choice
  (ask "Why does the inner loop test i < got and not buf[i] != '\\0'?")
  (right "fs_read_chunk adds no 0 byte, and the bytes after got are left over from earlier")
  (wrong "A file cannot hold the character '\\n'"
    "Text files are full of '\\n'; that is what the program counts. The point is where this piece of data ends.")
  (wrong "They are the same thing"
    "fs_mallocfile adds a 0 byte; fs_read_chunk does not. Only got says where the data ends.")
  (hint "Does fs_read_chunk put a 0 after what it read?"))
```

---

## Close it, and writing in pieces

`fs_close_handle(h)` gives the handle back. The system keeps a small
table of open files, so a program that forgets to close them can use
it up. Close on every path out, errors included.

Writing works the same way: `fs_open_write(path)` makes or replaces
the file and gives back a handle, `fs_write_chunk(h, buf, len)` adds
len bytes to the end and gives back how many it wrote, and
fs_close_handle finishes.

```c
int fs_open_write(const char *path);
int fs_write_chunk(int h, const void *buf, int len);
```

```sources
(source "sw/common/zfsapp.h, fs_close_handle: a leaked slot in a small, bounded table")
```

```check
(choice
  (ask "A program opens a file, finds a mistake in it, prints a message and returns 1 straight away. What has it forgotten?")
  (right "To close the handle before returning")
  (wrong "To free the buffer"
    "buf is a global array, not memory from malloc; it is not freed. The open file's handle is what must be given back.")
  (wrong "Nothing: returning closes it"
    "Close on every path out. A handle left open holds a place in the system's small table of open files.")
  (hint "What did fs_open_read give the program that must be given back?"))
```

So: open to get a handle, read or write in pieces until the count
says the end, and always close the handle.
