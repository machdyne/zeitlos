#include "zsys.h"

struct point { int x, y; };
struct box { struct point tl, br; char name[8]; };

union u32b { unsigned u; unsigned char b[4]; };

enum color { RED, GREEN, BLUE = 10, PURPLE };

int g_int = 42;
int g_arr[5] = { 1, 2, 3, 4, 5 };
char g_str[] = "global";
const char *g_ptr = "pointer";
struct point g_pt = { 3, 4 };
int g_bss[4];

typedef struct point point_t;

static int area(struct box *b) {
    return (b->br.x - b->tl.x) * (b->br.y - b->tl.y);
}

int main(void) {
    line("g_int", g_int);
    line("g_arr2", g_arr[2]);
    puts_("g_str ");
    puts_(g_str);
    putch('\n');
    puts_("g_ptr ");
    puts_(g_ptr);
    putch('\n');
    line("g_pt", g_pt.x * 10 + g_pt.y);
    line("g_bss", g_bss[3]);

    g_bss[3] = 99;
    line("g_bss2", g_bss[3]);

    {
        struct box b;
        b.tl.x = 1; b.tl.y = 2;
        b.br.x = 11; b.br.y = 12;
        line("area", area(&b));
        line("sizeof_box", (int)sizeof(struct box));
        line("sizeof_pt", (int)sizeof(point_t));
    }

    {
        union u32b u;
        u.u = 0x11223344u;
        line("union0", u.b[0]);
        line("union3", u.b[3]);
    }

    line("enum", RED + GREEN + BLUE + PURPLE);

    {
        int a[4];
        int *p = a;
        int i;
        for (i = 0; i < 4; i++) a[i] = i * i;
        line("ptr0", *p);
        line("ptr2", *(p + 2));
        p += 3;
        line("ptr3", *p);
        line("ptrdiff", (int)(p - a));
        line("idx", a[1] + p[0]);
    }

    {
        char buf[16];
        int i;
        for (i = 0; i < 5; i++) buf[i] = (char)('A' + i);
        buf[5] = 0;
        puts_("buf ");
        puts_(buf);
        putch('\n');
    }

    {
        char local[] = "local";
        puts_("local ");
        puts_(local);
        putch('\n');
        line("sizeof_local", (int)sizeof(local));
    }

    {
        struct point a, b;
        a.x = 7; a.y = 8;
        b = a;                  /* struct assignment */
        line("structcopy", b.x * 10 + b.y);
    }

    {
        int z[6] = { 1, 2, 3 };  /* partial init, rest zeroed */
        line("partial", z[0] + z[1] + z[2] + z[3] + z[4] + z[5]);
    }
    return 0;
}
