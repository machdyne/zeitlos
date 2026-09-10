#include "zsys.h"

int main(void) {
    line("add", 3 + 4);
    line("sub", 10 - 25);
    line("mul", 6 * 7);
    line("div", 100 / 7);
    line("rem", 100 % 7);
    line("neg", -(3 * 3));
    line("prec", 2 + 3 * 4 - 6 / 2);
    line("paren", (2 + 3) * 4);
    line("shl", 1 << 10);
    line("sar", -256 >> 4);
    line("and", 0xf0f0 & 0x0ff0);
    line("or", 0xf000 | 0x000f);
    line("xor", 0xffff ^ 0x0f0f);
    line("not", ~0);
    line("lt", 3 < 4);
    line("ge", 3 >= 4);
    line("eq", 5 == 5);
    line("ne", 5 != 5);
    line("land", 1 && 0);
    line("lor", 1 || 0);
    line("lnot", !0);
    line("cond", 1 ? 11 : 22);
    line("cond2", 0 ? 11 : 22);

    {
        unsigned u = 0xfffffff0u;
        puts_("ushr ");
        putu(u >> 4);
        putch('\n');
        puts_("udiv ");
        putu(u / 16u);
        putch('\n');
        line("ucmp", u > 5);
    }
    return 0;
}
