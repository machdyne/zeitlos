#include "zsys.h"
#include "t05_inc.h"

#define ANSWER 42
#define SQR(x) ((x) * (x))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CAT(a, b) a##b
#define STR(x) #x
#define XSTR(x) STR(x)
#define EMPTY
#define RECURSE ANSWER

#define REG_BASE 0x1000
#define REG(n) (REG_BASE + (n) * 4)

#if defined(ANSWER) && ANSWER == 42
#define GOOD 1
#else
#define GOOD 0
#endif

#ifndef NOT_DEFINED
#define FROM_IFNDEF 7
#endif

int CAT(my, var) = 5;

int main(void) {
    line("answer", ANSWER);
    line("sqr", SQR(1 + 2));
    line("max", MAX(3, 9));
    line("cat", myvar);
    line("reg", REG(3));
    line("good", GOOD);
    line("ifndef", FROM_IFNDEF);
    line("recurse", RECURSE);
    line("included", INCLUDED_VALUE);
    line("guarded", GUARD_ONCE);
    puts_("str ");
    puts_(XSTR(ANSWER));
    putch('\n');
    puts_("strlit ");
    puts_(STR(hello world));
    putch('\n');
    EMPTY
#if 2 + 2 == 4 && !defined(NOPE)
    line("arith_if", 1);
#else
    line("arith_if", 0);
#endif
#if 0
    this is not even valid C and must never be seen
#elif 1
    line("elif", 1);
#else
    line("elif", 0);
#endif
    return 0;
}
