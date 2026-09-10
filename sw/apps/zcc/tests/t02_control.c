#include "zsys.h"

int fib(int n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}

int classify(int n) {
    switch (n) {
    case 0:
    case 1:
        return 100;
    case 7:
        return 700;
    default:
        return -1;
    }
}

int main(void) {
    int i, sum;

    sum = 0;
    for (i = 0; i < 10; i++) sum += i;
    line("for", sum);

    sum = 0;
    i = 0;
    while (i < 5) { sum += i * i; i++; }
    line("while", sum);

    sum = 0;
    i = 0;
    do { sum++; i++; } while (i < 3);
    line("do", sum);

    sum = 0;
    for (i = 0; i < 10; i++) {
        if (i == 3) continue;
        if (i == 7) break;
        sum += i;
    }
    line("brkcont", sum);

    line("fib10", fib(10));
    line("sw0", classify(0));
    line("sw7", classify(7));
    line("swd", classify(42));

    i = 0;
again:
    i++;
    if (i < 4) goto again;
    line("goto", i);

    {
        int nested = 0;
        int a, b;
        for (a = 0; a < 3; a++)
            for (b = 0; b < 3; b++) {
                if (b == 2) break;
                nested++;
            }
        line("nested", nested);
    }
    return 0;
}
