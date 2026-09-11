# zcc examples

Three programs, in the order to try them.

## hello.c -- no headers, no runtime

```
zcc -nolibz -o /hello /hello.c
run hello
```

The only file that has to be on the card is `hello.c` itself. If this
works, the compiler, the ZEXE image and the loader are all sound --
which makes it the right first test when something is wrong.

Output goes to the **serial console**.

## hello_libz.c -- with the runtime

```
zcc -I /libz/include -L /libz -o /hellz /hello_libz.c
run hellz
```

`printf`, `malloc`, the string functions. Output still goes to the
serial console: that is where a program's stdout goes on this machine.

## hello_term.c -- output to the term window

```
zcc -I /libz/include -L /libz -o /apps/hello /hello_term.c
hello
```

The same program, printing where the user is actually looking.

**A program's output does not reach the terminal unless it asks.**
`posix` relays for a program that opens a second connection tagged
`"stdout"` -- about fifteen lines, and `zcc` itself does exactly this
(`sw/apps/zcc/port_dev.c`). A program that does not ask keeps printing
to the console, which is right when there is no `posix` to ask.

Two details the example shows and the reason it is worth reading
rather than copying blindly:

- **Send CRLF, not LF.** The bytes reach a VT100 with nothing in
  between to expand them. `printf()` to the console does that
  expansion in `_write()`, so the fallback path looks the same and
  behaves differently.
- **Batch if you produce much output.** `z_port_send()` refuses once
  eight messages are unacked and the excess is DROPPED. One send per
  line is fine for a few lines and loses the tail of a screenful --
  see `docs/ports.md`, which is the trap everything in this tree has
  hit at least once.
