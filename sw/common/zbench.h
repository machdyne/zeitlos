#ifndef ZBENCH_H
#define ZBENCH_H

/*
 * zbench.h -- the bench's protocol (docs/bench.md): what a client sends
 * the port provider bench0 to use the bench's I2C buses. zi2cx.c is the
 * client every app uses; only it and bench include this.
 *
 * One request per Z_PORT_DATA blob, one reply blob per request, in order.
 * A client connects with z_port_connect_arg(..., "zi2cx").
 *
 *   XFER   request  ZB_XFER, bus name, 0, addr, wn, w[wn], rn
 *          reply    status, written, r[rn] (only if status is ZB_ACK)
 *   LIST   request  ZB_LIST
 *          reply    bus names, each ending in 0, then one more 0
 *   ROLE   request  ZB_ROLE, role name, 0
 *          reply    the name of the bus with that role, 0 ("" if none)
 *
 * wn and rn are at most ZB_MAX. A write of nothing and a read of nothing
 * is an address probe: ACK if a part answers there.
 */

#include <stdint.h>
#include <string.h>

#define ZB_PROVIDER     "bench0"
#define ZB_TAG          "zi2cx"

#define ZB_XFER         1
#define ZB_LIST         2
#define ZB_ROLE         3       /* role name, 0 -> the bus with that role
                                   in the netlist, 0; "" if none: the
                                   BASIC computer's is "basic" */

/*
 * Modules (ls99, docs/ls99.md). bench starts one ls99 per module in its
 * netlist, with the module's name as its launch argument; ls99 connects
 * with "module:NAME". Messages go both ways on that connection, none of
 * them answered except SLAVE:
 *
 *   bench -> ls99
 *     HELLO   profile, Sechs address, local bus name 0, program path 0
 *     TIME    the virtual time, u32 ms little-endian: a module's WAIT and
 *             SLEEP wait until it has passed their end (its own clock
 *             only sleeping advances, so its timing is exact)
 *     LEVELS  8 levels: pins A-G and the LED's net (0, 1, 2 floating)
 *     SLAVE   id, wn, w[wn], rn: a transaction to its Sechs side
 *     QUIT    the netlist is gone: end
 *   ls99 -> bench
 *     DRIVES  8 drives: pins A-G, LED (0 none, 1 low, 2 high)
 *     DONE    id, status, r[rn]: SLAVE's answer
 *     CONSOLE what it printed
 *     ADDR    its new Sechs address
 */
#define ZB_MODULE_TAG   "module:"
#define ZB_HELLO        10
#define ZB_TIME         11
#define ZB_LEVELS       12
#define ZB_SLAVE        13
#define ZB_QUIT         14
#define ZB_DRIVES       20
#define ZB_DONE         21
#define ZB_CONSOLE      22
#define ZB_ADDR         23
#define ZB_PINS         8       /* A-G and the LED */

#define ZB_MAX          64      /* bytes written or read in one transaction */
#define ZB_NAME         16      /* a bus name, with its 0 */

/* reply status */
#define ZB_ACK          0
#define ZB_NACK_ADDR    1
#define ZB_NACK_DATA    2
#define ZB_NO_BUS       3
#define ZB_BAD          4       /* a request bench could not read */

/* An XFER request into buf (at least ZB_NAME + ZB_MAX + 4 bytes): its
 * length, or -1 if a size is out of range. */
static inline int zb_xfer_encode(uint8_t *buf, const char *bus, uint8_t addr,
                                 const uint8_t *w, int wn, int rn) {
    int n = (int)strlen(bus);
    if (n >= ZB_NAME || wn < 0 || wn > ZB_MAX || rn < 0 || rn > ZB_MAX) return -1;
    buf[0] = ZB_XFER;
    memcpy(buf + 1, bus, (size_t)n + 1);
    n += 2;
    buf[n++] = addr;
    buf[n++] = (uint8_t)wn;
    if (wn) memcpy(buf + n, w, (size_t)wn);
    n += wn;
    buf[n++] = (uint8_t)rn;
    return n;
}

/* An XFER request out of buf: 0, or -1 if it is malformed. *bus points
 * into buf; *w too. */
static inline int zb_xfer_decode(const uint8_t *buf, int len, const char **bus,
                                 uint8_t *addr, const uint8_t **w, int *wn, int *rn) {
    int i = 1;
    if (len < 5 || buf[0] != ZB_XFER) return -1;
    while (i < len && i <= ZB_NAME && buf[i]) i++;
    if (i >= len || buf[i] || i == 1 || i > ZB_NAME) return -1;
    *bus = (const char *)buf + 1;
    i++;
    if (i + 2 > len) return -1;
    *addr = buf[i++];
    *wn = buf[i++];
    if (*wn > ZB_MAX || i + *wn + 1 != len) return -1;
    *w = buf + i;
    i += *wn;
    *rn = buf[i];
    return *rn > ZB_MAX ? -1 : 0;
}

#endif
