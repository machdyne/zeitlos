# System calls

```lesson
(objective "Describe how a Zeitlos program calls the kernel, and why that call is safe.")
(minutes 15)
(keywords "reg_kernel" "syscall number")
```

You will learn how a Zeitlos program asks the kernel for something.

On many systems a system call is a trap: a special instruction makes
the CPU jump into the kernel. Zeitlos does it more simply. At start-up
the kernel writes the address of its entry function into the boot RAM,
at `0x0000_000C`. This word is called **reg_kernel**. A program makes a
system call with an ordinary call through that pointer, passing a
**syscall number** that says what it wants, and a pointer to where the
answer goes.

```sources
(source "Zeitlos docs: kernel.md, The syscall boundary; app_runtime.md, The syscall trampoline")
```

```check
(choice
  (ask "How does a Zeitlos app enter the kernel for a system call?")
  (right "It calls, like any function, the address it reads from 0x0000_000C")
  (wrong "It runs a special trap instruction"
    "Zeitlos uses no trap for system calls. The entry's address is in reg_kernel, and the app simply calls it.")
  (wrong "It sends a message to process 0"
    "Messages are themselves sent with a system call. The call goes through the pointer in the boot RAM.")
  (hint "Not a trap."))
```

---

## Why a plain call is safe

If an app can call kernel code, what stops it from calling any kernel
code it likes? The MPU. Kernel code becomes privileged because of where
it is fetched from, and an app may enter it only at the gates. The
system call entry is one gate; the interrupt entry is the other.

Inside, the kernel checks what it was handed. A pointer that is
misaligned, or points where the caller may not write, is refused with a
line on the console, and the program carries on.

```sources
(source "Zeitlos docs: mpu.md, The rules (the gate); Syscall pointer checks; Test programs")
```

```check
(choice
  (ask "An app passes the system call entry a pointer into kernel memory, asking for an answer to be written there. What happens?")
  (right "The kernel refuses the call and prints a line saying so")
  (wrong "The MPU blocks the kernel's store"
    "The kernel is privileged, so the MPU would let it write. That is why the kernel checks pointers itself.")
  (wrong "The kernel writes there, because it trusts every app"
    "It used to be possible to crash the kernel that way. The kernel now checks every pointer it is given."))
```

---

## Numbers

Each system call has a number, from the list `sw/common/syscalls.def`,
such as `Z_SYS_UPTIME` (9), the ticks since start-up, and
`Z_SYS_MSG_SEND`, send a message. A program rarely uses the numbers
itself: the runtime library wraps each one in a function, such as
`z_uptime_ticks()`.

```sources
(source "Zeitlos docs: app_runtime.md, The syscall trampoline (the table); mpu.md, Test programs (UPTIME is 9)")
```

```check
(choice
  (ask "In which region is reg_kernel?")
  (right "Region 0, the boot RAM")
  (wrong "Region 4, main memory" "The kernel's code is in main memory, but the pointer to its entry is kept at 0x0000_000C, in the boot RAM.")
  (wrong "Region 9" "Region 9 is the MTU and MPU. 0x0000_000C begins with 0."))
```

---

```check
(choice
  (ask "Why does every Zeitlos program find the kernel's entry the same way, whatever kernel version it runs on?")
  (right "It reads the entry's address from the same fixed place, 0x0000_000C, at run time")
  (wrong "The kernel's entry is always at the same address in main memory"
    "The entry function moves when the kernel changes. The pointer to it is what stays in one place.")
  (wrong "Every program is rebuilt with each kernel"
    "Programs need no rebuilding for that: they read the pointer when they run."))
```

So: a Zeitlos system call is a plain call through the pointer at `0x0000_000C`, made safe by the MPU's gates and the kernel's own pointer checks.
