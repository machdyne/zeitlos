/*
 * zcc's own <stdint.h>.
 *
 * The start of the compiler's header set, which is deliberately its
 * own rather than borrowed from the host toolchain: newlib's headers
 * are full of GCC-specific attributes, builtins and __extension__ that
 * this compiler has no way to read, and the tree only ever needs the
 * fixed-width names.
 *
 * Sizes are the rv32/ilp32 ones: int and long are both 32 bits, which
 * is why int32_t is int and there is no int64_t -- see docs/zcc.md on
 * why `long long` is refused rather than silently truncated.
 */

#ifndef _STDINT_H
#define _STDINT_H

typedef signed char        int8_t;
typedef unsigned char      uint8_t;
typedef short              int16_t;
typedef unsigned short     uint16_t;
typedef int                int32_t;
typedef unsigned int       uint32_t;

typedef int                intptr_t;
typedef unsigned int       uintptr_t;
typedef int                intmax_t;
typedef unsigned int       uintmax_t;

#define INT8_MIN    (-128)
#define INT8_MAX    127
#define UINT8_MAX   255
#define INT16_MIN   (-32768)
#define INT16_MAX   32767
#define UINT16_MAX  65535
#define INT32_MIN   (-2147483647 - 1)
#define INT32_MAX   2147483647
#define UINT32_MAX  4294967295u

#endif
