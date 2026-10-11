# POSIX file I/O for apps

`open()`, `read()`, `write()`, `lseek()`, `fstat()`, `stat()`,
`unlink()`, `rename()`, `mkdir()`, `rmdir()`, `opendir()`, `chdir()`,
`getcwd()` -- and on top of them newlib's `fopen()`, `fgets()`,
`fprintf()` and the rest -- reach files in any app built with

```
POSIX_IO = 1
```

in its Makefile. The calls go to the filesystem driver `fs`
([fs_driver.md](fs_driver.md)) through its client, `zfsc.c`.
`sw/common/zposix.c` is the whole of it.

## Why opt in

newlib does its file I/O through a few low-level calls (`_open`,
`_read`, `_write`, `_close`, `_lseek`, `_fstat`). `sw/common/zeitlos.c`
has always defined them as weak stubs that fail -- except for
descriptors 0-2, which are the terminal. Nothing written for Zeitlos
needed more, because `zfsapp.h` is the native API. Ported code does
not know that, and `vi` and `posix` each grew their own adapter.

A real implementation needs the filesystem client, and linking that
into the runtime would put it in every binary, including the ones that
never touch a file. So it is a separate object that an app asks for,
about 4-5KB with `zfsc.o`. Its definitions are strong, and the linker
prefers them to the weak stubs.

## How it maps

| POSIX | `fs` (`zfsproto.h`) |
|---|---|
| descriptor `n` >= 3 | `fs`'s descriptor `n - 3`: no table in between, no limit but `fs`'s |
| descriptors 0, 1, 2 | not `fs`'s: `z_term_read()`/`z_term_write()` (zeitlos.c), the terminal hooks or the serial console, as before |
| `O_RDONLY` `O_WRONLY` `O_RDWR` `O_CREAT` `O_EXCL` `O_TRUNC` `O_APPEND` | the `Z_O_*` flags, one for one (`O_APPEND` starts at the end of the file) |
| `lseek` with `SEEK_SET`/`SEEK_CUR`/`SEEK_END` | `Z_FSOP_LSEEK` with the same whence |
| `errno` | the result negated: `fs`'s `-Z_E*` numbers are newlib's |
| `st_mode` | `S_IFDIR` 0755, or `S_IFREG` 0644, 0444 for a read-only file (a flash-only one) |
| `st_mtime` | the FAT timestamp (`z_fs_info_time()`) |
| `rmdir()` | `unlink` of an empty directory, `ENOTEMPTY` otherwise |
| `lstat()` | `stat()`: FAT has no links |
| `link()` | `EMLINK` |

**The working directory.** Each process has one, `/` at start, so
relative paths, `chdir()` and `getcwd()` work. `.` and `..` are
resolved before the path goes to `fs`, which takes only absolute
paths.

**Directories.** newlib's `<dirent.h>` for this target is a single
`#error`, so `POSIX_IO` puts `sw/common/posix` on the include path,
with a `dirent.h` whose `struct dirent` has `d_name`, `d_type`
(`DT_DIR`/`DT_REG`) and `d_ino`. `opendir()` holds one page of entries
at a time.

**One request per call.** A call costs a round trip to `fs`, about a
millisecond. A `read()` or `write()` moves up to 32KB in one request.
`stdio`'s buffering (1KB by default in newlib) turns many small
`fgetc()`s into few `read()`s. A program that reads a big file a byte
at a time should still `setvbuf()` a bigger buffer.

**Writes are gathered.** Under `stdio`, zfsc keeps an 8KB write-behind
buffer per program: after a descriptor's first `write()`, which goes
straight to `fs` so errors show at once, smaller writes are sent 8KB at
a time. Every other call on any descriptor sends the buffer first, and
a write that fails later is reported by the next `write()`, `fsync()`
or `close()` of that descriptor ([fs_driver.md](fs_driver.md), "Small
writes are gathered").

## Who uses it

- **`vi`.** It dropped the `fileio.c` it carried and the directory,
  `stat` and working-directory stubs, so `:e <tab>` completion and
  `:cd` work now.
- **`posix`** keeps its own `vfs.c` for now, because it also has mount
  points and pipes. It could sit on top of `zposix.c` later.

## Testing

`sw/apps/fs/tests` builds `zposix.c` with its names prefixed
(`-DZPOSIX_HOST_TEST`), so the host's C library stays intact
underneath. It runs it against `fs` itself, in both builds of the
client. Covered:

- relative paths, `.` and `..`, `chdir`, `getcwd`;
- create, write, `fstat`, read, seek from each end;
- `O_APPEND`, `O_RDWR` without truncation, `ftruncate`, `O_EXCL`;
- descriptors 0-2 going to the terminal;
- `opendir`/`readdir` with types;
- `rename`, `unlink`, `rmdir` (`ENOTEMPTY`, `ENOTDIR`).

`fopen()` and the rest of stdio are newlib's own code over these, so
they are not tested on the host. `vi` is the check on the board.
