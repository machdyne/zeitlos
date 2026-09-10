/* <stdarg.h> is a COMPILER header, not a libc one, so this just hands
 * the job back to GCC's builtins. It exists only because -nostdinc
 * removes the compiler's own include directory along with the system
 * one, and fmt.c genuinely needs real varargs -- see its comment on
 * why the RISC-V ABI makes GCC's va_arg agree with zcc's calling
 * convention. */
#ifndef _STDARG_H
#define _STDARG_H
typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)
#define va_end(ap)         __builtin_va_end(ap)
#define va_copy(d, s)      __builtin_va_copy(d, s)
#endif
