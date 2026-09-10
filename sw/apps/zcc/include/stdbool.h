#ifndef _STDBOOL_H
#define _STDBOOL_H

/* Macros rather than a _Bool type. zcc treats _Bool as unsigned char
 * for layout, but `bool` as a macro for int keeps every existing
 * `bool x = 1;` and `return true;` in the tree working without a
 * conversion rule this compiler does not have. */
#define bool  int
#define true  1
#define false 0
#define __bool_true_false_are_defined 1

#endif
