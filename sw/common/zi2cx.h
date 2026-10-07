#ifndef ZI2CX_H
#define ZI2CX_H

/*
 * zi2cx.h -- I2C buses by name: a PMOD port's real pins, or a bus on the
 * bench (docs/bench.md), with one interface, so an app's I2C code is the
 * same for both.
 *
 *   "pmod0" ... "pmodN"   zi2c on PMOD port N: pin 1 (bit 0) SCL, pin 2
 *                         (bit 1) SDA, as a Wolfszahn wires a module --
 *                         through bench instead while bench owns the
 *                         port (a gpio in its netlist), so the two never
 *                         drive the same pins
 *   any other name        a bus in the running bench's netlist, through
 *                         its port bench0 (zbench.h)
 *
 *   z_i2cx_t bus;
 *   if (z_i2cx_open(&bus, "main") != Z_I2C_OK) ...
 *   z_i2cx_write(&bus, 0x20, cmd, 3);
 *   z_i2cx_write_read(&bus, 0x20, &reg, 1, data, 2);
 *   z_i2cx_close(&bus);
 *
 * Results are zi2c's (Z_I2C_OK, Z_I2C_NACK, ...), plus Z_I2CX_NO_BENCH
 * and Z_I2CX_NO_BUS from z_i2cx_open().
 *
 * A bench transaction waits for bench's reply. Acks are handled; any
 * other message that arrives meanwhile goes to `other`, if set (an app
 * with a window must not lose wm's messages), and is dropped otherwise.
 * One transaction at a time per z_i2cx_t.
 */

#include <stdint.h>
#include <stdbool.h>
#include "zi2c.h"
#include "zport.h"

#define Z_I2CX_NO_BENCH 100     /* a bench bus, but no bench running */
#define Z_I2CX_NO_BUS   101     /* no bus of that name */

typedef struct {
    bool bench;
    char name[16];
    z_i2c_t pins;               /* a PMOD port */
    z_port_t port;              /* bench0 */
    void (*other)(z_msg_t *msg);
} z_i2cx_t;

int z_i2cx_open(z_i2cx_t *b, const char *name);

/* The bench bus the running netlist gives this role ("basic": the BASIC
 * computer's pins 3 and 4, from `basic BUS`), opened: Z_I2CX_NO_BENCH,
 * Z_I2CX_NO_BUS if no bus has the role, or z_i2cx_open()'s result. */
int z_i2cx_open_role(z_i2cx_t *b, const char *role);
void z_i2cx_close(z_i2cx_t *b);

int z_i2cx_write(z_i2cx_t *b, uint8_t addr, const uint8_t *d, uint32_t n);
int z_i2cx_read(z_i2cx_t *b, uint8_t addr, uint8_t *d, uint32_t n);
int z_i2cx_write_read(z_i2cx_t *b, uint8_t addr, const uint8_t *w, uint32_t wn,
                      uint8_t *r, uint32_t rn);
int z_i2cx_probe(z_i2cx_t *b, uint8_t addr);    /* Z_I2C_OK if it answers */

/* The bench's buses, as names each ending in 0 and one more 0 after the
 * last: their number, or -1 if no bench is running. */
int z_i2cx_bench_buses(char *buf, int len);

#endif
