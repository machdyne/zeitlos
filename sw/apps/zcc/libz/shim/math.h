#ifndef _MATH_H
#define _MATH_H
/* zobj.c includes <math.h> for one line: an epsilon comparison of two
 * Z_FLOAT32 values in z_obj_equal(). That is the only floating point
 * anywhere in libz.
 *
 * fabsf as a static inline rather than a declaration, because there is
 * no libm here and pulling one in for a sign-bit clear would be
 * absurd. The bit trick is exact -- clearing bit 31 of an IEEE-754
 * single IS its absolute value, for every input including zero,
 * infinity and NaN -- and it compiles to one `andi`-style mask with no
 * call at all.
 *
 * The comparison itself still needs libgcc's __ltsf2, which is the
 * compiler's own support library and not a C library. See the Makefile. */
static inline float fabsf(float x) {
    union { float f; unsigned u; } v;
    v.f = x;
    v.u &= 0x7fffffffu;
    return v.f;
}
#endif
