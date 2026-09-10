#include "zsys.h"

static int str_len(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (int)(p - s);
}

static void str_copy(char *d, const char *s) {
    while ((*d++ = *s++)) ;
}

static int str_cmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static void str_rev(char *s) {
    int i = 0, j = str_len(s) - 1;
    while (i < j) {
        char t = s[i];
        s[i] = s[j];
        s[j] = t;
        i++;
        j--;
    }
}

int main(void) {
    char buf[32];

    line("len", str_len("hello"));
    str_copy(buf, "zeitlos");
    puts_("copy ");
    puts_(buf);
    putch('\n');
    line("cmp_eq", str_cmp("abc", "abc"));
    line("cmp_lt", str_cmp("abc", "abd") < 0);
    str_rev(buf);
    puts_("rev ");
    puts_(buf);
    putch('\n');

    {
        /* escapes, and signed plain char */
        char c = '\xff';
        line("signedchar", c);
        line("tab", '\t');
        line("octal", '\101');
        line("nl", '\n');
    }

    {
        /* a small bubble sort: arrays, loops, swaps, comparisons */
        int a[8];
        int i, j, n = 8;
        int seed = 12345;
        for (i = 0; i < n; i++) {
            seed = seed * 1103515245 + 12345;
            a[i] = (seed >> 16) & 0xff;
        }
        for (i = 0; i < n - 1; i++)
            for (j = 0; j < n - 1 - i; j++)
                if (a[j] > a[j + 1]) {
                    int t = a[j];
                    a[j] = a[j + 1];
                    a[j + 1] = t;
                }
        puts_("sorted");
        for (i = 0; i < n; i++) { putch(' '); putd(a[i]); }
        putch('\n');
        {
            int ok = 1;
            for (i = 0; i < n - 1; i++) if (a[i] > a[i + 1]) ok = 0;
            line("ordered", ok);
        }
    }
    return 0;
}
