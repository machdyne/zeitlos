/*
 * libz -- syscalls, and the hardware access zcc cannot emit.
 *
 * Every call here goes through reg_kernel, exactly as sw/common/
 * zeitlos.c does: a syscall on this machine is an ordinary function
 * call through a pointer the kernel installed at 0x0000000c, not a
 * trap. See docs/app_runtime.md, "The syscall trampoline".
 *
 * These are deliberately NOT #included from sw/common/zeitlos.c. That
 * file is written against newlib -- it defines _read/_write/_sbrk and
 * pulls in errno -- and this runtime has no newlib. The wrappers are
 * small and the duplication is visible; sharing them would mean
 * dragging a libc in behind them.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>        /* malloc/free, from shim/ -- fs_mallocfile() */
#include <string.h>        /* from shim/ */

/*
 * Compiled against sw/common's headers (with the shims), NOT against
 * libz.h. These functions ARE the ones zeitlos.h and zfsapp.h declare,
 * so they must be declared by those headers and defined here with
 * exactly matching types -- a `unsigned` where the header says
 * `uint32_t` is the same type until the include path changes, and then
 * it is a compile error in whichever file is unlucky.
 */
/* __zcc__ makes zeitlos.h DECLARE maskirq rather than define it as a
 * static inline -- which is exactly what this file needs, since the
 * definition it wants is the one below. The macro is not a claim to be
 * zcc; it selects the "maskirq comes from the runtime" arm of that
 * header, and this file IS the runtime. See docs/libz.md. */
#define __zcc__ 1
#include "zeitlos.h"
#undef __zcc__
#include "zfsapp.h"
#include "libz_int.h"

typedef z_obj_t zobj_t;

/* reg_kernel is zeitlos.h's own macro for the register at 0x0c. Used
 * as-is rather than redefined here, which is the same rule the rest of
 * this file follows: sw/common owns these names. */
#define REG_KERNEL (reg_kernel)

/* z_obj_t type tags, from sw/common/zobj.h. Duplicated as constants
 * rather than by including that header, for the same reason as above:
 * this file must build freestanding. */

void *z_syscall(unsigned id, void *arg) {
    z_kernel_ptr_t k = (z_kernel_ptr_t)(uintptr_t)REG_KERNEL;
    return k(id, (uint32_t *)arg, 0);
}

static int syscall_ok(unsigned id, void *arg) {
    zobj_t *rv = (zobj_t *)z_syscall(id, arg);
    return rv && rv->val.uint32 == Z_OK;
}

/* Syscall ids. Same numbering as sw/common/syscalls.def, which is the
 * ABI -- see that file's warning about never inserting in the middle. */
enum {
    ZS_EXIT = 1, ZS_UI_PRINT, ZS_UART_GETC, ZS_UART_PUTC,
    ZS_UART_RX_EMPTY, ZS_UART_TX_FULL, ZS_MSG_SEND, ZS_MSG_READ,
    ZS_UPTIME, ZS_HID_READ_KEY, ZS_PID_REGISTER, ZS_PID_LOOKUP,
    ZS_GETPID, ZS_PROC_RUN, ZS_FS_SIZE, ZS_FS_READ, ZS_FS_WRITE,
    ZS_FS_UNLINK, ZS_FS_LIST, ZS_FS_OPEN_WRITE, ZS_FS_OPEN_READ,
    ZS_FS_READ_CHUNK, ZS_FS_WRITE_CHUNK, ZS_FS_CLOSE, ZS_PROC_KILL,
    ZS_FS_MKDIR, ZS_FS_TOUCH, ZS_FS_SEEK, ZS_PROC_LIST, ZS_MEM_STATS,
    ZS_FS_DF, ZS_PROC_WAIT, ZS_EXEC_EXISTS
};

void z_exit(int status) {
    zobj_t o;
    o.type = Z_INT32;
    o.val.int32 = status;
    z_syscall(ZS_EXIT, &o);
    for (;;) ;                  /* the kernel does not return from this */
}

void libz_finish(void) {
    /* Falling off the end of main() is a normal exit. Getting here and
     * NOT exiting would run whatever bytes follow the entry stub, so
     * this is the backstop rather than an optimisation. */
    z_exit(0);
}

int putchar(int c) {
    zobj_t o;
    o.type = Z_INT32;
    o.val.int32 = c;
    z_syscall(ZS_UART_PUTC, &o);
    return c;
}

int getchar(void) {
    zobj_t o;
    o.type = Z_NONE;
    o.val.uint32 = 0;
    z_syscall(ZS_UART_GETC, &o);
    return (int)o.val.int32;
}

int kbhit(void) {
    zobj_t o;
    o.type = Z_NONE;
    o.val.uint32 = 0;
    z_syscall(ZS_UART_RX_EMPTY, &o);
    return !o.val.uint32;
}

uint32_t z_getpid(void) {
    zobj_t o;
    o.type = Z_NONE;
    o.val.uint32 = 0;
    z_syscall(ZS_GETPID, &o);
    return o.val.uint32;
}

uint32_t z_uptime_ticks(void) {
    zobj_t o;
    o.type = Z_NONE;
    o.val.uint32 = 0;
    z_syscall(ZS_UPTIME, &o);
    return o.val.uint32;
}

uint32_t z_proc_run(const char *name) {
    zobj_t o;
    o.type = Z_STR;
    o.val.str = (char *)name;
    z_syscall(ZS_PROC_RUN, &o);
    return o.val.uint32;
}

void z_proc_wait(uint32_t ticks) {
    zobj_t o;
    o.type = Z_UINT32;
    o.val.uint32 = ticks;
    z_syscall(ZS_PROC_WAIT, &o);
}

z_rv z_proc_kill(uint32_t pid) {
    zobj_t o;
    o.type = Z_UINT32;
    o.val.uint32 = pid;
    return syscall_ok(ZS_PROC_KILL, &o) ? Z_OK : Z_FAIL;
}

bool z_pid_register(const char *base, char *out, uint32_t cap) {
    zobj_t o;
    o.type = Z_STR;
    o.val.str = (char *)base;
    if (!syscall_ok(ZS_PID_REGISTER, &o)) return 0;
    if (out && cap > 0) {
        const char *s = o.val.str;
        int i = 0;
        while (s && s[i] && (uint32_t)i < cap - 1) { out[i] = s[i]; i++; }
        out[i] = 0;
    }
    return 1;
}

bool z_pid_lookup(const char *name, uint32_t *pid) {
    zobj_t o;
    o.type = Z_STR;
    o.val.str = (char *)name;
    if (!syscall_ok(ZS_PID_LOOKUP, &o)) return 0;
    if (pid) *pid = o.val.uint32;
    return 1;
}

/* z_msg_send/z_msg_read/z_msg_wait/z_msg_new_send live in glue.c, not
 * here. They take a z_msg_t, and sw/common/zwin.c calls them with that
 * type -- so they have to be declared exactly as sw/common/zeitlos.h
 * declares them, which means being compiled against that header rather
 * than against libz.h. See glue.c's own comment. */

/* -- filesystem --
 *
 * The argument structs are flat runs of 32-bit fields, defined in
 * sw/common/zfs.h. Declared locally for the same freestanding reason
 * as everything else here; the LAYOUT is the contract and is checked
 * by the test suite against the real header.
 */

typedef struct { const char *name; unsigned size; } fs_size_args;
typedef struct { const char *name; void *buf; unsigned cap; unsigned len; } fs_read_args;
typedef struct { const char *name; const void *buf; unsigned len; unsigned written; } fs_write_args;
typedef struct { const char *name; } fs_path_args;
typedef struct { const char *name; int handle; } fs_open_args;
typedef struct { int handle; void *buf; unsigned cap; unsigned len; } fs_chunk_args;
typedef struct { int handle; unsigned offset; unsigned pos; } fs_seek_args;
typedef struct { int handle; } fs_handle_args;

int fs_size(char *path) {
    fs_size_args a;
    a.name = path;
    a.size = 0;
    z_syscall(ZS_FS_SIZE, &a);
    return (int)a.size;
}

int fs_read_into(const char *path, void *buf, int cap) {
    fs_read_args a;
    a.name = path;
    a.buf = buf;
    a.cap = (unsigned)cap;
    a.len = 0;
    if (!syscall_ok(ZS_FS_READ, &a)) return -1;
    return (int)a.len;
}

int fs_write_file(char *path, char *buf, int len) {
    fs_write_args a;
    a.name = path;
    a.buf = buf;
    a.len = (unsigned)len;
    a.written = 0;
    if (!syscall_ok(ZS_FS_WRITE, &a)) return -1;
    return (int)a.written;
}

int fs_unlink(char *path) {
    fs_path_args a;
    a.name = path;
    return syscall_ok(ZS_FS_UNLINK, &a) ? 0 : -1;
}

int fs_mkdir(const char *path) {
    fs_path_args a;
    a.name = path;
    return syscall_ok(ZS_FS_MKDIR, &a) ? 0 : -1;
}

static int fs_open_common(unsigned id, const char *path) {
    fs_open_args a;
    a.name = path;
    a.handle = -1;
    if (!syscall_ok(id, &a)) return -1;
    return a.handle;
}

int fs_open_read(const char *path)  { return fs_open_common(ZS_FS_OPEN_READ, path); }
int fs_open_write(const char *path) { return fs_open_common(ZS_FS_OPEN_WRITE, path); }

/*
 * The whole-file helper the tree calls fs_mallocfile(): read a file
 * into a fresh malloc'd buffer, NUL-terminated, or NULL.
 *
 * Named to match sw/common/zfsapp.h rather than inventing fs_read(),
 * so that a program moving between a GCC build and a zcc build does
 * not change a single call. That principle -- the runtime provides the
 * tree's OWN names, not a parallel API -- is the thing that keeps
 * libz.h from becoming a second set of headers to keep in step.
 */
char *fs_mallocfile(char *path) {
    int sz = fs_size(path);
    if (sz <= 0) return 0;
    char *buf = malloc((unsigned)sz + 1);
    if (!buf) return 0;
    if (fs_read_into(path, buf, sz) != sz) { free(buf); return 0; }
    buf[sz] = 0;
    return buf;
}

int fs_read_chunk(int handle, void *buf, int cap) {
    fs_chunk_args a;
    a.handle = handle;
    a.buf = buf;
    a.cap = (unsigned)cap;
    a.len = 0;
    if (!syscall_ok(ZS_FS_READ_CHUNK, &a)) return -1;
    return (int)a.len;
}

int fs_write_chunk(int handle, const void *buf, int len) {
    fs_chunk_args a;
    a.handle = handle;
    a.buf = (void *)buf;
    a.cap = (unsigned)len;
    a.len = 0;
    if (!syscall_ok(ZS_FS_WRITE_CHUNK, &a)) return -1;
    return (int)a.len;
}

int fs_seek(int handle, uint32_t offset) {
    fs_seek_args a;
    a.handle = handle;
    a.offset = offset;
    a.pos = 0;
    if (!syscall_ok(ZS_FS_SEEK, &a)) return -1;
    return (int)a.pos;
}

int fs_close_handle(int handle) {
    fs_handle_args a;
    a.handle = handle;
    return syscall_ok(ZS_FS_CLOSE, &a) ? 0 : -1;
}

/*
 * maskirq -- the reason this table exists.
 *
 * A raw picorv32 custom instruction, identical to the one in
 * sw/common/zeitlos.h. zcc has no inline assembler and is not getting
 * one; here it is ordinary GCC-built code behind an ordinary table
 * slot. See docs/zcc.md, "The maskirq problem", and note that the same
 * treatment works for anything else the compiler cannot express.
 *
 * Always restore the PREVIOUS mask, never a hardcoded unmask-all --
 * this can be nested inside an already-masked outer context.
 */
uint32_t maskirq(uint32_t new_mask) {
    unsigned old_mask;
    __asm__ volatile (
        ".insn r 0x0B, 0x6, 0x03, %0, %1, zero"
        : "=r"(old_mask)
        : "r"(new_mask)
        : "memory"
    );
    return old_mask;
}
