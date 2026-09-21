/*
 * zfpga -- the relative-name resolver and the router (Phase 4).
 *
 * THE RESOLVER. Trellis names a wire relative to the tile that lists
 * it. resolve() turns (tile, name) into a global wire exactly as
 * libtrellis's RoutingGraph::globalise_net_ecp5() does; tools/
 * resolvecheck.py proves the same rules against every pip nextpnr used
 * in four designs, 16,000-odd arcs, with no exceptions (docs/zfpga.md
 * sec. 13.1).
 *
 * THE GRAPH IS NEVER BUILT. Resolved, the 25F database is 7.7 million
 * arcs; this never holds more than the wires one search touches. What a
 * wire drives is found on demand: the database uses only a few dozen
 * distinct relative prefixes (N1, E3, S13E2, ...), so for each prefix
 * the tile that would see the wire under that name is looked up, and
 * that tile type's arcs are searched by source name through an index
 * built the first time the type is met.
 *
 * THE ROUTER, v2. Negotiated congestion (PathFinder, McMurchie and
 * Ebeling 1995): every net is routed by A*, sink by sink, from the tree
 * it already has; nets may share a wire at a price, and the price of a
 * shared wire rises each iteration -- the present-sharing factor
 * doubles, and a wire overused at the end of an iteration accrues
 * history cost for good -- until no wire has two users. Costs are
 * integers: the CPU has no FPU.
 *
 * Global wires (G_, L_, R_) are excluded: clocks go over general
 * routing, which PLC2's clock muxes accept, and is right for small
 * designs only.
 *
 * v1 formatted a relative name and binary-searched it by string for
 * every offset of every wire it expanded -- 59% of its time in the
 * formatter and strcmp (docs/zfpga.md sec. 14.1). v2 parses every name
 * once, into (row offset, column offset, interned base), keys each tile
 * type's index by those integers, and keeps for each base name only the
 * offsets it actually appears under.
 */

#include "zfpga.h"

/* -- wires -------------------------------------------------------------
 * A global wire is (row, col, name). Names are interned: every basename
 * the resolver can produce appears somewhere in the database's string
 * table, so an interned name is a string-table offset. */

static zdb_t *db;
static int max_row, max_col;
static char prefix[5];

/* string -> string-table offset, for interning basenames */
static uint32_t *strhash;
static uint32_t strhash_cap;

static uint32_t hash_str(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

static void strhash_build(void) {
    const char *p = db->str, *end = db->str + db->hdr->sect[ZDB_S_STR].count;
    uint32_t n = 0;
    for (; p < end; p += zf_strlen(p) + 1) n++;
    strhash_cap = 1;
    while (strhash_cap < n * 2) strhash_cap <<= 1;
    strhash = zf_alloc(sizeof(uint32_t) * strhash_cap);
    for (p = db->str; p < end; p += zf_strlen(p) + 1) {
        uint32_t off = (uint32_t)(p - db->str), i;
        if (!off) continue;             /* "" */
        for (i = hash_str(p) & (strhash_cap - 1); strhash[i]; i = (i + 1) & (strhash_cap - 1))
            if (zf_streq(db->str + strhash[i], p)) break;
        strhash[i] = off;
    }
}

/* 0 if the name is nowhere in the database (so no arc can use it) */
static uint32_t intern(const char *s) {
    uint32_t i;
    for (i = hash_str(s) & (strhash_cap - 1); strhash[i]; i = (i + 1) & (strhash_cap - 1))
        if (zf_streq(db->str + strhash[i], s)) return strhash[i];
    return 0;
}

static int is_global(const char *n) {
    return (n[0] == 'G' || n[0] == 'L' || n[0] == 'R') && n[1] == '_';
}

/* globalise_net_ecp5(). Returns 0 for "not a wire on this die". */
int resolve(int row, int col, const char *name, gwire_t *out) {
    char buf[96];
    const char *p;
    int n;
    if (name[0] >= '0' && name[0] <= '9' && name[1] >= '0' && name[1] <= '9' &&
            name[2] == 'K' && name[3] == '_') {
        if (name[0] != prefix[0] || name[1] != prefix[1]) return 0;
        name += 4;
    }
    if (col >= 69) {
        /* libtrellis: PCSA and PCSB share tile databases */
        const char *q;
        size_t len = zf_strlen(name);
        for (q = name; *q; q++)
            if (q[0] == 'P' && q[1] == 'C' && q[2] == 'S' && q[3] == 'A') break;
        if (*q && len < sizeof(buf)) {
            zf_memcpy(buf, name, len + 1);
            buf[q - name + 3] = 'B';
            name = buf;
        }
    }
    if (is_global(name)) {
        int nominal = name[0] == 'G';
        const char *q;
        for (q = name; *q && nominal; q++)
            if ((q[0] == 'V' && q[1] == 'P' && q[2] == 'T' && q[3] == 'X') ||
                (q[0] == 'H' && q[1] == 'P' && q[2] == 'B' && q[3] == 'X') ||
                (q[0] == 'H' && q[1] == 'P' && q[2] == 'R' && q[3] == 'X'))
                nominal = 0;
        out->row = (int16_t)(nominal ? 0 : row);
        out->col = (int16_t)(nominal ? 0 : col);
        out->name = intern(name);
        return out->name != 0;
    }
    /* ^([NS]\d+)?([EW]\d+)?_(.*)$ */
    p = name;
    {
        int r = row, c = col, ok = 1;
        if (*p == 'N' || *p == 'S') {
            int s = *p == 'N' ? -1 : 1;
            p++;
            if (*p < '0' || *p > '9') ok = 0;
            for (n = 0; *p >= '0' && *p <= '9'; p++) n = n * 10 + (*p - '0');
            r += s * n;
        }
        if (ok && (*p == 'E' || *p == 'W')) {
            int s = *p == 'W' ? -1 : 1;
            p++;
            if (*p < '0' || *p > '9') ok = 0;
            for (n = 0; *p >= '0' && *p <= '9'; p++) n = n * 10 + (*p - '0');
            c += s * n;
        }
        if (ok && p != name && *p == '_') {
            row = r;
            col = c;
            name = p + 1;
        }
    }
    if (row < 0 || row > max_row || col < 0 || col > max_col) return 0;
    out->row = (int16_t)row;
    out->col = (int16_t)col;
    out->name = intern(name);
    return out->name != 0;
}

/* -- where tiles are ------------------------------------------------------ */

static int32_t *loc_first;      /* (row, col) -> first tile index there, or -1 */
static int32_t *loc_next;       /* tile -> next tile at the same location */
static int16_t *tile_row, *tile_col;

static void geometry(void) {
    uint32_t t;
    int cells = (max_row + 1) * (max_col + 1), i;
    loc_first = zf_alloc(sizeof(int32_t) * (size_t)cells);
    loc_next = zf_alloc(sizeof(int32_t) * db->n_tile);
    tile_row = zf_alloc(sizeof(int16_t) * db->n_tile);
    tile_col = zf_alloc(sizeof(int16_t) * db->n_tile);
    for (i = 0; i < cells; i++) loc_first[i] = -1;
    for (t = db->n_tile; t-- > 0;) {
        const char *n = ZDB_STR(db, db->tile[t].name);
        int r = 0, c = 0;
        while (*n && !(n[0] == 'R' && n[1] >= '0' && n[1] <= '9')) n++;
        if (!*n) zf_fatal("tile %s has no location", ZDB_STR(db, db->tile[t].name));
        for (n++; *n >= '0' && *n <= '9'; n++) r = r * 10 + (*n - '0');
        if (*n++ != 'C') zf_fatal("tile %s: bad location", ZDB_STR(db, db->tile[t].name));
        for (; *n >= '0' && *n <= '9'; n++) c = c * 10 + (*n - '0');
        tile_row[t] = (int16_t)r;
        tile_col[t] = (int16_t)c;
        loc_next[t] = loc_first[r * (max_col + 1) + c];
        loc_first[r * (max_col + 1) + c] = (int32_t)t;
    }
}

/* -- names, parsed once -------------------------------------------------
 * A database name is local (an offset and a base) or global. Parsed per
 * string-table offset and cached. PCSA/PCSB names (DCU tiles only) are
 * marked unusable rather than given resolve()'s column-dependent rule:
 * the router never goes near a transceiver. */

typedef struct { int8_t dy, dx; uint8_t global; uint32_t base; } pname_t;

static uint32_t *pn_key, *pn_idx, pn_cap;
static pname_t *pn;
static uint32_t n_pn, cap_pn;

static const pname_t *parse(uint32_t off) {
    uint32_t i;
    const char *name, *p;
    pname_t r;
    int n;
    for (i = (off * 2654435761u) & (pn_cap - 1); pn_key[i]; i = (i + 1) & (pn_cap - 1))
        if (pn_key[i] == off) return &pn[pn_idx[i]];
    zf_memset(&r, 0, sizeof(r));
    name = db->str + off;
    if (name[0] >= '0' && name[0] <= '9' && name[1] >= '0' && name[1] <= '9' &&
            name[2] == 'K' && name[3] == '_') {
        if (name[0] != prefix[0] || name[1] != prefix[1]) r.global = 2;
        name += 4;
    }
    for (p = name; *p && r.global != 2; p++)
        if (p[0] == 'P' && p[1] == 'C' && p[2] == 'S' && (p[3] == 'A' || p[3] == 'B')) r.global = 2;
    if (r.global != 2 && is_global(name)) r.global = 1;
    if (!r.global) {
        int dy = 0, dx = 0;
        p = name;
        if (*p == 'N' || *p == 'S') {
            int sg = *p == 'N' ? -1 : 1;
            for (p++, n = 0; *p >= '0' && *p <= '9'; p++) n = n * 10 + (*p - '0');
            dy = sg * n;
        }
        if (*p == 'E' || *p == 'W') {
            int sg = *p == 'W' ? -1 : 1;
            for (p++, n = 0; *p >= '0' && *p <= '9'; p++) n = n * 10 + (*p - '0');
            dx = sg * n;
        }
        if (p != name && *p == '_' && p[-1] >= '0' && p[-1] <= '9') {
            r.dy = (int8_t)dy;
            r.dx = (int8_t)dx;
            name = p + 1;
        }
        r.base = intern(name);
        if (!r.base) r.global = 2;
    }
    if (n_pn == cap_pn) {
        pname_t *np = zf_alloc(sizeof(pname_t) * cap_pn * 2);
        zf_memcpy(np, pn, sizeof(pname_t) * n_pn);
        pn = np;
        cap_pn *= 2;
    }
    pn[n_pn] = r;
    pn_key[i] = off;
    pn_idx[i] = n_pn++;
    if (n_pn * 2 > pn_cap) zf_fatal("router: name cache full");
    return &pn[n_pn - 1];
}

/* -- base name -> the offsets it is named under ---------------------------- */

typedef struct { int8_t dy, dx; } boff_t;
typedef struct { uint32_t base; boff_t *o; uint16_t n, cap; } bofs_t;
static bofs_t *bo;
static uint32_t bo_cap;

static bofs_t *bofs(uint32_t base, int create) {
    uint32_t i;
    for (i = (base * 2654435761u) & (bo_cap - 1); bo[i].base; i = (i + 1) & (bo_cap - 1))
        if (bo[i].base == base) return &bo[i];
    if (!create) return NULL;
    bo[i].base = base;
    return &bo[i];
}

static void bofs_add(uint32_t base, int dy, int dx) {
    bofs_t *b = bofs(base, 1);
    int k;
    for (k = 0; k < b->n; k++)
        if (b->o[k].dy == dy && b->o[k].dx == dx) return;
    if (b->n == b->cap) {
        uint16_t cap = (uint16_t)(b->cap ? b->cap * 2 : 4);
        boff_t *no = zf_alloc(sizeof(boff_t) * cap);
        if (b->n) zf_memcpy(no, b->o, sizeof(boff_t) * b->n);
        b->o = no;
        b->cap = cap;
    }
    b->o[b->n].dy = (int8_t)dy;
    b->o[b->n].dx = (int8_t)dx;
    b->n++;
}

/* -- per-type index, keyed by (source offset, source base) ----------------- */

#define NO_ARC 0xffffffffu
typedef struct {
    uint64_t key;           /* source: base | (dy+128) << 32 | (dx+128) << 40 */
    int8_t sdy, sdx;        /* sink, relative to the tile */
    uint32_t sbase;
    uint32_t sink;          /* sink name, for the emitted arc */
    uint32_t arc;           /* NO_ARC: a fixed connection */
} rev_t;
typedef struct { rev_t *e; uint32_t n; int built; } revidx_t;
static revidx_t *rev;

static uint64_t rkey(int dy, int dx, uint32_t base) {
    return (uint64_t)base | ((uint64_t)(dy + 128) << 32) | ((uint64_t)(dx + 128) << 40);
}

static void rev_sort(rev_t *e, uint32_t n) {
    /* shell sort: no qsort, to keep newlib's out of the device build */
    uint32_t gap, i, j;
    for (gap = n / 2; gap > 0; gap /= 2)
        for (i = gap; i < n; i++) {
            rev_t t = e[i];
            for (j = i; j >= gap && e[j - gap].key > t.key; j -= gap) e[j] = e[j - gap];
            e[j] = t;
        }
}

static void rev_add(revidx_t *r, uint32_t src, uint32_t sink, uint32_t arc) {
    const pname_t *ps = parse(src), *pk = parse(sink);
    rev_t *e;
    if (ps->global || pk->global) return;       /* v2: no global wires */
    e = &r->e[r->n++];
    e->key = rkey(ps->dy, ps->dx, ps->base);
    e->sdy = pk->dy;
    e->sdx = pk->dx;
    e->sbase = pk->base;
    e->sink = sink;
    e->arc = arc;
}

static revidx_t *rev_of(uint32_t type) {
    revidx_t *r = &rev[type];
    const zdb_type_t *ty = &db->type[type];
    uint32_t m, a, n = ty->n_fix;
    if (r->built) return r;
    for (m = 0; m < ty->n_mux; m++) n += db->mux[ty->mux0 + m].n_arc;
    r->e = zf_alloc(sizeof(rev_t) * (n ? n : 1));
    for (m = 0; m < ty->n_mux; m++) {
        const zdb_mux_t *mx = &db->mux[ty->mux0 + m];
        for (a = 0; a < mx->n_arc; a++)
            rev_add(r, db->arc[mx->arc0 + a].source, mx->sink, mx->arc0 + a);
    }
    for (m = 0; m < ty->n_fix; m++)
        rev_add(r, db->fix[ty->fix0 + m].source, db->fix[ty->fix0 + m].sink, NO_ARC);
    rev_sort(r->e, r->n);
    r->built = 1;
    return r;
}

/* -- wires the router has touched -------------------------------------------- */

typedef struct {
    gwire_t w;
    int32_t term;           /* net whose terminal this is, or -1 */
    uint16_t occ;           /* nets currently using it */
    uint16_t hist;          /* accumulated history cost, fixed point /16 */
    int32_t prev;           /* search: node we came from */
    uint32_t via_tile, via_arc, via_sink;
    uint32_t cost, stamp, tree;
    uint8_t closed;
} node_t;

static node_t *nodes;
static uint32_t n_nodes, nodes_cap;
static uint32_t *nhash, nhash_cap;
static uint32_t stamp, tree_serial;

static uint32_t hash_w(const gwire_t *w) {
    return ((uint32_t)w->row * 73856093u) ^ ((uint32_t)w->col * 19349663u) ^ (w->name * 83492791u);
}

static void nhash_grow(void) {
    uint32_t cap = nhash_cap * 2, k;
    uint32_t *nh = zf_alloc(sizeof(uint32_t) * cap);
    for (k = 0; k < n_nodes; k++) {
        uint32_t i;
        for (i = hash_w(&nodes[k].w) & (cap - 1); nh[i]; i = (i + 1) & (cap - 1)) ;
        nh[i] = k + 1;
    }
    nhash = nh;
    nhash_cap = cap;
}

static int32_t node_of(const gwire_t *w) {
    uint32_t i;
    for (i = hash_w(w) & (nhash_cap - 1); nhash[i]; i = (i + 1) & (nhash_cap - 1)) {
        node_t *n = &nodes[nhash[i] - 1];
        if (n->w.row == w->row && n->w.col == w->col && n->w.name == w->name)
            return (int32_t)(nhash[i] - 1);
    }
    if (n_nodes == nodes_cap) {
        node_t *nn = zf_alloc(sizeof(node_t) * nodes_cap * 2);
        zf_memcpy(nn, nodes, sizeof(node_t) * n_nodes);
        nodes = nn;
        nodes_cap *= 2;
    }
    zf_memset(&nodes[n_nodes], 0, sizeof(node_t));
    nodes[n_nodes].w = *w;
    nodes[n_nodes].term = -1;
    nodes[n_nodes].prev = -1;
    nhash[i] = ++n_nodes;
    if (n_nodes * 2 > nhash_cap) nhash_grow();
    return (int32_t)(n_nodes - 1);
}

/* -- the open set -------------------------------------------------------- */

typedef struct { uint32_t f; int32_t n; } hent_t;
static hent_t *heap;
static uint32_t heap_n, heap_cap;

static void heap_push(uint32_t f, int32_t n) {
    uint32_t i;
    if (heap_n == heap_cap) {
        hent_t *nh = zf_alloc(sizeof(hent_t) * heap_cap * 2);
        zf_memcpy(nh, heap, sizeof(hent_t) * heap_n);
        heap = nh;
        heap_cap *= 2;
    }
    i = heap_n++;
    while (i && heap[(i - 1) / 2].f > f) { heap[i] = heap[(i - 1) / 2]; i = (i - 1) / 2; }
    heap[i].f = f;
    heap[i].n = n;
}

static hent_t heap_pop(void) {
    hent_t top = heap[0], last = heap[--heap_n];
    uint32_t i = 0;
    for (;;) {
        uint32_t c = 2 * i + 1;
        if (c >= heap_n) break;
        if (c + 1 < heap_n && heap[c + 1].f < heap[c].f) c++;
        if (heap[c].f >= last.f) break;
        heap[i] = heap[c];
        i = c;
    }
    if (heap_n) heap[i] = last;
    return top;
}

/* -- per-net trees ---------------------------------------------------------- */

typedef struct { int32_t node; uint32_t tile, arc, sink; } tent_t;
typedef struct { tent_t *e; int n, cap; } tree_t;
static tree_t *trees;

static void tree_add(int ni, int32_t node, uint32_t tile, uint32_t arc, uint32_t sink) {
    tree_t *t = &trees[ni];
    if (t->n == t->cap) {
        int cap = t->cap ? t->cap * 2 : 16;
        tent_t *ne = zf_alloc(sizeof(tent_t) * (size_t)cap);
        if (t->n) zf_memcpy(ne, t->e, sizeof(tent_t) * (size_t)t->n);
        t->e = ne;
        t->cap = cap;
    }
    t->e[t->n].node = node;
    t->e[t->n].tile = tile;
    t->e[t->n].arc = arc;
    t->e[t->n].sink = sink;
    t->n++;
}

static void rip_up(int ni) {
    int k;
    for (k = 1; k < trees[ni].n; k++) nodes[trees[ni].e[k].node].occ--;
    trees[ni].n = 0;
}

/* -- search ----------------------------------------------------------------- */

static int iabs(int x) { return x < 0 ? -x : x; }

#define UNIT 16

static uint32_t hdist(const gwire_t *a, const gwire_t *b) {
    return (uint32_t)(iabs(a->row - b->row) + iabs(a->col - b->col)) * UNIT;
}

static uint32_t pres_fac;       /* /16 */

static uint32_t wire_cost(const node_t *n) {
    uint32_t base = UNIT + n->hist;
    return base * (UNIT + pres_fac * n->occ) / UNIT;
}

static int32_t search(int ni, const gwire_t *target, int r0, int c0, int r1, int c1,
        uint32_t *expanded) {
    int k;
    stamp++;
    heap_n = 0;
    for (k = 0; k < trees[ni].n; k++) {
        node_t *n = &nodes[trees[ni].e[k].node];
        n->stamp = stamp;
        n->cost = 0;
        n->prev = -1;
        n->closed = 0;
        heap_push(hdist(&n->w, target), trees[ni].e[k].node);
    }
    while (heap_n) {
        hent_t h = heap_pop();
        gwire_t cw = nodes[h.n].w;
        uint32_t ccost = nodes[h.n].cost;
        bofs_t *b;
        int o;
        if (nodes[h.n].closed) continue;
        nodes[h.n].closed = 1;
        if (cw.row == target->row && cw.col == target->col && cw.name == target->name)
            return h.n;
        (*expanded)++;
        b = bofs(cw.name, 0);
        if (!b) continue;
        for (o = 0; o < b->n; o++) {
            int tr = cw.row - b->o[o].dy, tc = cw.col - b->o[o].dx;
            uint64_t key = rkey(b->o[o].dy, b->o[o].dx, cw.name);
            int32_t t;
            if (tr < 0 || tr > max_row || tc < 0 || tc > max_col) continue;
            if (tr < r0 - 1 || tr > r1 + 1 || tc < c0 - 1 || tc > c1 + 1) continue;
            for (t = loc_first[tr * (max_col + 1) + tc]; t >= 0; t = loc_next[t]) {
                revidx_t *ri = rev_of(db->tile[t].type);
                int32_t lo = 0, hi = (int32_t)ri->n - 1, first = -1;
                while (lo <= hi) {
                    int32_t mid = (lo + hi) / 2;
                    if (ri->e[mid].key >= key) { if (ri->e[mid].key == key) first = mid; hi = mid - 1; }
                    else lo = mid + 1;
                }
                for (; first >= 0 && (uint32_t)first < ri->n && ri->e[first].key == key; first++) {
                    const rev_t *e = &ri->e[first];
                    gwire_t nw;
                    int32_t nn;
                    uint32_t nc;
                    nw.row = (int16_t)(tr + e->sdy);
                    nw.col = (int16_t)(tc + e->sdx);
                    nw.name = e->sbase;
                    if (nw.row < r0 || nw.row > r1 || nw.col < c0 || nw.col > c1) continue;
                    nn = node_of(&nw);
                    if (nodes[nn].term >= 0 && nodes[nn].term != ni) continue;
                    if (nodes[nn].tree == tree_serial) continue;
                    nc = ccost + wire_cost(&nodes[nn]);
                    if (nodes[nn].stamp == stamp && (nodes[nn].closed || nodes[nn].cost <= nc))
                        continue;
                    nodes[nn].stamp = stamp;
                    nodes[nn].cost = nc;
                    nodes[nn].prev = h.n;
                    nodes[nn].closed = 0;
                    nodes[nn].via_tile = (uint32_t)t;
                    nodes[nn].via_arc = e->arc;
                    nodes[nn].via_sink = e->sink;
                    heap_push(nc + hdist(&nw, target), nn);
                }
            }
        }
    }
    return -1;
}

/* Routes one net from scratch. Returns the first sink that failed, or -1. */
static int route_one(int ni, const rnet_t *nt, uint32_t *expanded) {
    int s, margin, r0, r1, c0, c1;
    int32_t src = node_of(&nt->src);
    tree_serial++;
    trees[ni].n = 0;
    tree_add(ni, src, 0, NO_ARC, 0);
    nodes[src].tree = tree_serial;
    for (s = 0; s < nt->n_sinks; s++) {
        int32_t n, k;
        int32_t tn = node_of(&nt->sinks[s]);
        if (nodes[tn].tree == tree_serial) continue;
        /* a tight window first; a wide one if that fails */
        for (margin = 4; margin <= 16; margin += 12) {
            int j;
            r0 = r1 = nt->src.row;
            c0 = c1 = nt->src.col;
            for (j = 0; j < nt->n_sinks; j++) {
                if (nt->sinks[j].row < r0) r0 = nt->sinks[j].row;
                if (nt->sinks[j].row > r1) r1 = nt->sinks[j].row;
                if (nt->sinks[j].col < c0) c0 = nt->sinks[j].col;
                if (nt->sinks[j].col > c1) c1 = nt->sinks[j].col;
            }
            r0 -= margin; c0 -= margin; r1 += margin; c1 += margin;
            if (r0 < 0) r0 = 0;
            if (c0 < 0) c0 = 0;
            if (r1 > max_row) r1 = max_row;
            if (c1 > max_col) c1 = max_col;
            n = search(ni, &nt->sinks[s], r0, c0, r1, c1, expanded);
            if (n >= 0) break;
        }
        if (n < 0) return s;
        for (k = n; k >= 0 && nodes[k].tree != tree_serial; k = nodes[k].prev) {
            nodes[k].tree = tree_serial;
            nodes[k].occ++;
            tree_add(ni, k, nodes[k].via_tile, nodes[k].via_arc, nodes[k].via_sink);
        }
    }
    return -1;
}

void route_init(zdb_t *d) {
    const char *dev;
    uint32_t t, a, types;
    db = d;
    max_row = 0;
    max_col = 0;
    dev = db->device;
    /* 25K_ / 45K_ / 85K_: the 12F routes as a 25F */
    if (dev[6] == '1' && dev[7] == '2') { prefix[0] = '2'; prefix[1] = '5'; }
    else { prefix[0] = dev[6]; prefix[1] = dev[7]; }
    prefix[2] = 'K'; prefix[3] = '_'; prefix[4] = 0;
    strhash_build();
    for (t = 0; t < db->n_tile; t++) {
        const char *n = ZDB_STR(db, db->tile[t].name);
        int r = 0, c = 0;
        while (*n && !(n[0] == 'R' && n[1] >= '0' && n[1] <= '9')) n++;
        for (n++; *n >= '0' && *n <= '9'; n++) r = r * 10 + (*n - '0');
        if (*n == 'C') for (n++; *n >= '0' && *n <= '9'; n++) c = c * 10 + (*n - '0');
        if (r > max_row) max_row = r;
        if (c > max_col) max_col = c;
    }
    geometry();

    pn_cap = 1 << 15;
    pn_key = zf_alloc(sizeof(uint32_t) * pn_cap);
    pn_idx = zf_alloc(sizeof(uint32_t) * pn_cap);
    cap_pn = 1024;
    pn = zf_alloc(sizeof(pname_t) * cap_pn);
    bo_cap = 1 << 13;
    bo = zf_alloc(sizeof(bofs_t) * bo_cap);

    /* The offsets each base name is SEEN under, as a source of an arc or
     * of a fixed connection, anywhere on the die. Fixed connections
     * count: a top PIOB's data arrives over PIOT0's `JPADDOB <=
     * S1E1_JA0`, and no configurable arc uses S1E1. */
    for (a = 0; a < db->hdr->sect[ZDB_S_ARC].count + db->hdr->sect[ZDB_S_FIX].count; a++) {
        uint32_t so = a < db->hdr->sect[ZDB_S_ARC].count ? db->arc[a].source
            : db->fix[a - db->hdr->sect[ZDB_S_ARC].count].source;
        const pname_t *p = parse(so);
        if (!p->global) bofs_add(p->base, p->dy, p->dx);
    }

    types = db->hdr->sect[ZDB_S_TYPE].count;
    rev = zf_alloc(sizeof(revidx_t) * types);
    nodes_cap = 4096;
    nodes = zf_alloc(sizeof(node_t) * nodes_cap);
    nhash_cap = 8192;
    nhash = zf_alloc(sizeof(uint32_t) * nhash_cap);
    heap_cap = 4096;
    heap = zf_alloc(sizeof(hent_t) * heap_cap);
}

/* Parses "R2C5/MUXCLK0" into a global wire. */
int route_parse_wire(const char *s, gwire_t *w) {
    int r = 0, c = 0;
    const char *p = s;
    if (*p++ != 'R' || *p < '0' || *p > '9') return 0;
    for (; *p >= '0' && *p <= '9'; p++) r = r * 10 + (*p - '0');
    if (*p++ != 'C' || *p < '0' || *p > '9') return 0;
    for (; *p >= '0' && *p <= '9'; p++) c = c * 10 + (*p - '0');
    if (*p++ != '/') return 0;
    if (r > max_row || c > max_col) return 0;
    w->row = (int16_t)r;
    w->col = (int16_t)c;
    w->name = intern(p);
    return w->name != 0;
}

const char *route_wire_str(const gwire_t *w) {
    static char buf[96];
    zf_fmt(buf, sizeof(buf), "R%dC%d/%s", w->row, w->col, db->str + w->name);
    return buf;
}

/*
 * The PathFinder loop. Returns -1 when every net is routed and no wire
 * is shared; otherwise the index of a net that could not be routed at
 * all (*sink says which sink), or -2 if sharing never resolved.
 */
int route_all(const rnet_t *nets, int n_nets, arc_cb emit, route_stats_t *st, int *sink) {
    int i, iter, k;
    trees = zf_alloc(sizeof(tree_t) * (size_t)(n_nets ? n_nets : 1));
    zf_memset(st, 0, sizeof(*st));

    /* terminals: a net's pins are its own, never another's */
    for (i = 0; i < n_nets; i++) {
        int32_t n = node_of(&nets[i].src);
        if (nodes[n].term >= 0 && nodes[n].term != i) { *sink = -1; return i; }
        nodes[n].term = i;
        for (k = 0; k < nets[i].n_sinks; k++) {
            n = node_of(&nets[i].sinks[k]);
            if (nodes[n].term >= 0 && nodes[n].term != i) { *sink = k; return i; }
            nodes[n].term = i;
        }
    }

    pres_fac = 8;               /* 0.5 */
    for (iter = 1; iter <= 50; iter++) {
        uint32_t over = 0;
        st->iterations = iter;
        for (i = 0; i < n_nets; i++) {
            int shared = 0;
            if (iter > 1) {
                for (k = 1; k < trees[i].n; k++)
                    if (nodes[trees[i].e[k].node].occ > 1) { shared = 1; break; }
                if (!shared) continue;
                rip_up(i);
            }
            st->reroutes++;
            k = route_one(i, &nets[i], &st->expanded);
            if (k >= 0) { *sink = k; return i; }
        }
        for (k = 0; k < (int)n_nodes; k++)
            if (nodes[k].occ > 1) {
                over++;
                if (nodes[k].hist < 60000) nodes[k].hist += UNIT;
            }
        st->overused = over;
        if (!over) break;
        if (pres_fac < 4096) pres_fac *= 2;
    }
    if (st->overused) return -2;

    for (i = 0; i < n_nets; i++)
        for (k = 1; k < trees[i].n; k++) {
            const tent_t *e = &trees[i].e[k];
            st->arcs += e->arc != NO_ARC;
            if (e->arc != NO_ARC)
                emit(i, e->tile, db->str + e->sink, db->str + db->arc[e->arc].source);
        }
    st->wires = n_nodes;
    return -1;
}
