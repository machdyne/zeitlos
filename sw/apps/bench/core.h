#ifndef BENCH_CORE_H
#define BENCH_CORE_H

/*
 * bench -- the core: nets, parts, buses and the transaction log
 * (docs/bench.md). Portable: nothing here touches Zeitlos, so it is
 * tested on a host (tests/). One bench per process: the state is here,
 * not passed around.
 *
 * A NET is resolved from the drives of the part pins on it:
 *
 *   one or more pins driving high or low     that level; both: CONFLICT
 *   no strong drive, pull-ups or pull-downs  that level; both: FLOATING
 *   nothing at all                           FLOATING
 *
 * (an open-drain output is a pin that drives low or nothing). When a
 * level changes, the parts on the net are told (inputs()), and may
 * change their own drives in turn.
 *
 * A BUS carries I2C transactions, each atomic: write, read, or write then
 * read with a repeated start. The part at the address gets its bytes in
 * one call per phase, then the stop.
 */

#include <stdint.h>
#include <stdbool.h>

#define BN_PARTS    64      /* parts in a netlist */
#define BN_NETS     256
#define BN_BUSES    8
#define BN_PINS     24      /* pins of one part */
#define BN_NAME     16
#define BN_LABEL    24
#define BN_STATE    64      /* bytes of state a part may keep */
#define BN_LOG      64      /* transactions remembered */

/* a net's level */
#define BN_L0       0
#define BN_L1       1
#define BN_FLOAT    2
#define BN_CONFLICT 3

/* a pin's drive */
#define BN_NONE     0
#define BN_LOW      1
#define BN_HIGH     2
#define BN_PULLUP   3
#define BN_PULLDOWN 4

/* a transaction's outcome */
#define BN_ACK          0
#define BN_NACK_ADDR    1   /* nobody at that address */
#define BN_NACK_DATA    2   /* the part refused a byte */

typedef struct part part_t;
typedef struct param param_t;

/* What makes a part type: one C file in parts/ fills one of these in. */
typedef struct {
    const char *name;               /* "tca9535" */
    const char *about;              /* one line, for the documentation */
    const char *const *pins;        /* pin names */
    uint8_t npins;
    uint8_t addr_lo, addr_hi;       /* I2C addresses; 0, 0 if not on a bus */
    /* parameters checked, state set up; 0, or an error message */
    const char *(*init)(part_t *p, const param_t *params);
    /* I2C: the bytes written after its address (the bytes it ACKed), the
     * bytes read, the stop */
    int (*i2c_write)(part_t *p, const uint8_t *d, int n);
    void (*i2c_read)(part_t *p, uint8_t *d, int n);
    void (*i2c_stop)(part_t *p);
    /* a net it is on changed level */
    void (*inputs)(part_t *p);
    /* a click on it (buttons and switches): pressed or released */
    void (*click)(part_t *p, bool down);
    /* what it shows: on for an LED or a load (drawing is not here) */
    bool (*lit)(part_t *p);
    uint8_t addr_dflt;              /* addr= if none is given; 0: needed */
    uint8_t module;                 /* a Zwölf module (LS99): 1 + its profile */
} part_type_t;

struct part {
    const part_type_t *type;
    char name[BN_NAME];
    char label[BN_LABEL];
    int16_t net[BN_PINS];           /* -1: not connected */
    uint8_t drive[BN_PINS];
    uint8_t addr;                   /* on a bus */
    int8_t bus;                     /* -1: none */
    int8_t col, row;                /* place; -1: automatic */
    uint8_t state[BN_STATE];        /* the type's own, zeroed at first */
};

typedef struct {
    char name[BN_NAME];
    uint8_t pull;                   /* BN_NONE, BN_PULLUP, BN_PULLDOWN */
    uint8_t level;
} net_t;

typedef struct {
    char name[BN_NAME];
    int8_t master;                  /* the module whose C/D it is; -1: none */
} bus_t;

typedef struct {
    uint32_t ms;                    /* the time */
    uint8_t bus, addr, status;
    uint8_t wn, rn;                 /* bytes written (as asked) and read */
    uint8_t w[8], r[8];             /* the first of them */
} xfer_log_t;

/* the bench */
extern part_t bn_parts[BN_PARTS];
extern int bn_nparts;
extern net_t bn_nets[BN_NETS];
extern int bn_nnets;
extern bus_t bn_buses[BN_BUSES];
extern int bn_nbuses;
extern xfer_log_t bn_log[BN_LOG];
extern int bn_log_next;             /* bn_log is a ring; this is next */
extern uint32_t bn_now;             /* milliseconds, set by the app */
extern int bn_conflicts;            /* times a net found a conflict */

void bn_clear(void);

/* for parts: their pins */
int bn_pin_get(part_t *p, int pin);             /* a level */
void bn_pin_drive(part_t *p, int pin, int drive);
#define BN_STATE_OF(p, type) ((type *)(void *)(p)->state)

/* for the netlist and the app */
int bn_net_find(const char *name);              /* -1 if none */
int bn_net_new(const char *name);               /* -1 if full */
void bn_net_merge(int keep, int gone);          /* gone's pins join keep */
void bn_net_resolve(int n);                     /* and tell its parts */
void bn_resolve_all(void);
part_t *bn_part_find(const char *name);
int bn_bus_find(const char *name);

/* A transaction on bus b: wn bytes written (none: a read only), then rn
 * read (none: a write only). Returns BN_ACK or a NACK; *written gets the
 * bytes ACKed. Logged. */
int bn_xfer(int b, uint8_t addr, const uint8_t *w, int wn, uint8_t *r, int rn,
            int *written);

/* A transaction addressed to a module (an LS99) on a bus: answered by
 * whoever runs it -- in-process for the tests; bench_app.c forwards it
 * to the module's own process instead. BN_ACK or BN_NACK_*. */
extern int (*bn_module_xfer)(part_t *p, const uint8_t *w, int wn, uint8_t *r, int rn);

/* a module's program= (parts/module.c) */
const char *bn_module_program(part_t *p);

/* part types (parts/parts.c) */
const part_type_t *bn_type_find(const char *name);
extern const part_type_t *const bn_types[];

/* parameters, for a type's init() (netlist.c) */
const char *bn_param(const param_t *pp, const char *key);   /* 0 if absent */
bool bn_flag(const param_t *pp, const char *word);
long bn_param_num(const param_t *pp, const char *key, long dflt);  /* -1: bad */

#endif
