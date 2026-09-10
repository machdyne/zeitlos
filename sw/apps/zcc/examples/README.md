# zcc examples

Two programs, in the order to try them. See `docs/zcc_bringup.md`.

## hello.c -- no headers, no runtime

```
zcc -nolibz -o /hello /hello.c
run hello
```

The only file that has to be on the card is `hello.c` itself. If this
works, the compiler, the ZEXE image and the loader are all sound.

## hello_libz.c -- with the runtime

```
zcc -I /libz -I /common -I /include -L /libz -o /hellz /hello_libz.c
run hellz
```

Needs `/libz/libz.bin`, `/libz/libz.sym`, and the headers under
`/common` and `/include`. If `hello.c` works and this does not, the
problem is libz or its files rather than the compiler.

## Where the output goes

The **kernel console**, not the `term` window you typed the command
in. A spawned program's output is not routed back to the shell yet;
`docs/posix.md` covers why and what it would take.
