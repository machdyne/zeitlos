#include "zsys.h"

static int add8(int a, int b, int c, int d, int e, int f, int g, int h) {
    return a + b + c + d + e + f + g + h;
}

static int apply(int (*fn)(int), int v) { return fn(v); }
static int twice(int v) { return v * 2; }
static int square(int v) { return v * v; }

typedef int (*intfn)(int);
static intfn table[2];

static int counter(void) {
    static int n = 100;
    n++;
    return n;
}

static void swap(int *a, int *b) { int t = *a; *a = *b; *b = t; }

int forward(int n);              /* prototype before the definition */

int main(void) {
    line("add8", add8(1, 2, 3, 4, 5, 6, 7, 8));
    line("fnptr", apply(twice, 21));

    table[0] = twice;
    table[1] = square;
    line("tbl0", table[0](5));
    line("tbl1", table[1](5));

    line("static1", counter());
    line("static2", counter());
    line("static3", counter());

    {
        int a = 3, b = 9;
        swap(&a, &b);
        line("swap", a * 10 + b);
    }

    line("forward", forward(6));

    {
        int x = 5;
        line("post", x++);
        line("postafter", x);
        line("pre", ++x);
        x -= 2;
        line("compound", x);
        x *= 3;
        line("compound2", x);
    }

    {
        int a[3] = { 10, 20, 30 };
        int i = 0;
        a[i++] += 5;             /* the index must be evaluated once */
        line("cmpidx", a[0]);
        line("cmpidx_i", i);
    }
    return 0;
}

int forward(int n) { return n * 7; }
