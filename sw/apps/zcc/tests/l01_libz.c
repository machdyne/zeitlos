#include "libz.h"

struct item { const char *name; int qty; };

int main(void) {
    printf("hello from libz\n");
    printf("dec %d  neg %d  uns %u  hex %x  HEX %X\n", 42, -7, 4000000000u, 48879, 48879);
    printf("width [%5d] [%-5d] [%05d]\n", 42, 42, 42);
    printf("str [%s] [%8s] [%-8s] [%.3s]\n", "abc", "abc", "abc", "abcdef");
    printf("char %c pct %% ptr %p\n", 'Z', (void *)0x80001234);

    {
        char buf[64];
        int n = snprintf(buf, sizeof(buf), "%s=%d/%d", "ratio", 22, 7);
        printf("snprintf %d [%s]\n", n, buf);
    }

    {
        char *p = malloc(100);
        char *q = malloc(200);
        strcpy(p, "malloc works");
        printf("%s len=%u\n", p, (unsigned)strlen(p));
        free(p);
        {
            char *r = malloc(64);
            printf("reuse=%d q=%d\n", r != 0, q != 0);
            free(r);
        }
        free(q);
        printf("heap used %u\n", (unsigned)z_heap_used());
    }

    {
        struct item items[3];
        int i, total = 0;
        items[0].name = "bolt";  items[0].qty = 3;
        items[1].name = "nut";   items[1].qty = 12;
        items[2].name = "washer";items[2].qty = 7;
        for (i = 0; i < 3; i++) {
            printf("  %-8s %3d\n", items[i].name, items[i].qty);
            total += items[i].qty;
        }
        printf("total %d\n", total);
    }

    printf("strcmp %d %d\n", strcmp("abc", "abc"), strcmp("abc", "abd") < 0);
    printf("strstr %s\n", strstr("zeitlos runtime", "runtime"));
    printf("atoi %d\n", atoi("  -1234xyz"));

    {
        unsigned old = maskirq(0xffffffffu);
        maskirq(old);
        printf("maskirq survived\n");
    }
    return 0;
}
