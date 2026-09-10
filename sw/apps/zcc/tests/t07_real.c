#include "zsys.h"

/*
 * Something closer to real code than the feature tests: a fixed-pool
 * allocator, a linked list, a hash table and a small state machine.
 * The point is the interaction between features, which is where a
 * one-pass compiler is most likely to be wrong.
 */

#define POOL_NODES 32
#define NBUCKETS   8

struct node {
    struct node *next;          /* self-referential, needs an incomplete type */
    const char *key;
    unsigned hash;
    int value;
};

static struct node pool[POOL_NODES];
static int pool_used;
static struct node *buckets[NBUCKETS];

static struct node *node_alloc(void) {
    if (pool_used >= POOL_NODES) return 0;
    return &pool[pool_used++];
}

static unsigned hash_of(const char *s) {
    unsigned h = 2166136261u;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h;
}

static void put(const char *key, int value) {
    unsigned h = hash_of(key);
    struct node *n = node_alloc();
    if (!n) return;
    n->key = key;
    n->hash = h;
    n->value = value;
    n->next = buckets[h % NBUCKETS];
    buckets[h % NBUCKETS] = n;
}

static int get(const char *key, int missing) {
    unsigned h = hash_of(key);
    struct node *n;
    for (n = buckets[h % NBUCKETS]; n; n = n->next) {
        if (n->hash != h) continue;
        {
            const char *a = n->key, *b = key;
            while (*a && *a == *b) { a++; b++; }
            if (*a == *b) return n->value;
        }
    }
    return missing;
}

/* a tiny tokenizer state machine, exercising switch inside a loop */
enum state { S_SPACE, S_WORD, S_NUM };

static int count_words(const char *s) {
    int words = 0;
    int st = S_SPACE;
    for (; *s; s++) {
        switch (st) {
        case S_SPACE:
            if (*s >= '0' && *s <= '9') { st = S_NUM; }
            else if (*s != ' ') { st = S_WORD; words++; }
            break;
        case S_WORD:
        case S_NUM:
            if (*s == ' ') st = S_SPACE;
            break;
        default:
            break;
        }
    }
    return words;
}

/* 2D array indexing and nested loops */
static int matrix_trace(void) {
    int m[4][4];
    int i, j, t = 0;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            m[i][j] = i * 4 + j;
    for (i = 0; i < 4; i++) t += m[i][i];
    return t;
}

struct op {
    const char *name;
    int (*fn)(int, int);
};

static int op_add(int a, int b) { return a + b; }
static int op_sub(int a, int b) { return a - b; }
static int op_mul(int a, int b) { return a * b; }

static struct op ops[3] = {
    { "add", op_add },
    { "sub", op_sub },
    { "mul", op_mul }
};

int main(void) {
    put("alpha", 1);
    put("beta", 2);
    put("gamma", 3);
    put("delta", 4);

    line("get_alpha", get("alpha", -1));
    line("get_delta", get("delta", -1));
    line("get_missing", get("epsilon", -1));
    line("pool_used", pool_used);

    line("words", count_words("  the quick 42 brown fox  "));
    line("trace", matrix_trace());

    {
        int i;
        for (i = 0; i < 3; i++) {
            puts_(ops[i].name);
            putch(' ');
            putd(ops[i].fn(12, 4));
            putch('\n');
        }
    }

    {
        /* pointer walking over a struct array */
        struct node *p = pool;
        struct node *end = pool + pool_used;
        int sum = 0;
        while (p < end) { sum += p->value; p++; }
        line("sumvalues", sum);
    }

    {
        /* deep expression nesting, to exercise the stack discipline */
        int a = 2, b = 3, c = 4, d = 5;
        line("deep", ((a + b) * (c - d)) + ((a * b) - (c + d)) * ((a - b) + (c * d)));
    }
    return 0;
}
