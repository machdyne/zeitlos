/*
 * zi2cx.c -- I2C buses by name (zi2cx.h).
 */

#include <string.h>
#include "zeitlos.h"
#include "zsoc.h"
#include "zi2cx.h"
#include "zbench.h"

#define REPLY_TICKS (Z_TICK_HZ)         /* bench answers at once; 1s is lost */

static bool pmod_name(const char *name, int *port) {
    if (strncmp(name, "pmod", 4) || !name[4]) return false;
    *port = 0;
    for (const char *p = name + 4; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        *port = *port * 10 + (*p - '0');
    }
    return true;
}

/* one request to bench, its reply into rbuf: the reply's length, or -1 */
static int ask(z_i2cx_t *b, const uint8_t *req, int len, uint8_t *rbuf, int rlen) {
    z_msg_t m;
    uint32_t start = z_uptime_ticks();
    if (z_port_send(&b->port, req, (uint32_t)len) != Z_OK) return -1;
    while (z_uptime_ticks() - start < REPLY_TICKS) {
        while (z_msg_read(&m) == Z_OK) {
            bool from_bench = b->port.connected && m.from == b->port.peer_pid;
            if (from_bench && m.subject == Z_PORT_DATA) {
                int n = (int)z_blob_len(&m.obj);
                if (n > rlen) n = rlen;
                memcpy(rbuf, z_blob_data(&m.obj), (size_t)n);
                z_port_send_ack(&m);
                return n;
            }
            if (from_bench && m.subject == Z_PORT_DATA_ACK) {
                z_port_handle_ack(&b->port, &m);
            } else if (from_bench && m.subject == Z_PORT_CLOSE) {
                z_port_close(&b->port);
                return -1;
            } else if (b->other) {
                b->other(&m);
            }
        }
        z_proc_wait(1);
    }
    return -1;
}

int z_i2cx_bench_buses(char *buf, int len) {
    z_i2cx_t b;
    uint8_t req = ZB_LIST;
    uint32_t pid = 0;
    int n, count = 0;
    memset(&b, 0, sizeof(b));
    if (!z_pid_lookup(ZB_PROVIDER, &pid) || !pid) return -1;
    if (z_port_connect_arg(&b.port, pid, z_obj_str(ZB_TAG)) != Z_OK) return -1;
    n = ask(&b, &req, 1, (uint8_t *)buf, len);
    z_port_close(&b.port);
    if (n < 1) return -1;
    buf[len - 1] = 0;
    for (char *p = buf; *p; p += strlen(p) + 1) count++;
    return count;
}

int z_i2cx_open_role(z_i2cx_t *b, const char *role) {
    z_i2cx_t q;
    uint8_t req[ZB_NAME + 2];
    char name[ZB_NAME + 1];
    uint32_t pid = 0;
    int n = (int)strlen(role);
    if (n >= ZB_NAME) return Z_I2CX_NO_BUS;
    memset(&q, 0, sizeof(q));
    if (!z_pid_lookup(ZB_PROVIDER, &pid) || !pid ||
        z_port_connect_arg(&q.port, pid, z_obj_str(ZB_TAG)) != Z_OK)
        return Z_I2CX_NO_BENCH;
    q.other = b->other;
    req[0] = ZB_ROLE;
    memcpy(req + 1, role, (size_t)n + 1);
    n = ask(&q, req, n + 2, (uint8_t *)name, ZB_NAME);
    z_port_close(&q.port);
    if (n < 1) return Z_I2CX_NO_BENCH;
    name[n < ZB_NAME ? n : ZB_NAME] = 0;
    if (!name[0]) return Z_I2CX_NO_BUS;
    void (*other)(z_msg_t *) = b->other;
    int r = z_i2cx_open(b, name);
    b->other = other;
    return r;
}

int z_i2cx_open(z_i2cx_t *b, const char *name) {
    int port;
    memset(b, 0, sizeof(*b));
    strncpy(b->name, name, sizeof(b->name) - 1);
    if (pmod_name(name, &port)) {
        /* a port bench owns (a gpio in its netlist) goes through bench,
         * which is then its only user (zgpio.h): bench lists it */
        char names[ZB_MAX * 4];
        uint32_t pid = 0;
        if (z_i2cx_bench_buses(names, sizeof(names)) > 0)
            for (char *p = names; *p; p += strlen(p) + 1)
                if (!strcmp(p, name) && z_pid_lookup(ZB_PROVIDER, &pid) && pid &&
                    z_port_connect_arg(&b->port, pid, z_obj_str(ZB_TAG)) == Z_OK) {
                    b->bench = true;
                    return Z_I2C_OK;
                }
        if (!z_gpio_present() || port >= (int)z_gpio_port_count()) return Z_I2CX_NO_BUS;
        b->pins.scl_port = b->pins.sda_port = (uint8_t)port;
        b->pins.scl_pin = 0;
        b->pins.sda_pin = 1;
        b->pins.khz = 100;
        b->pins.timeout_us = 1000;
        int r = z_i2c_init(&b->pins);
        if (r == Z_I2C_BUSY) r = z_i2c_recover(&b->pins);
        return r;
    }
    /* a bench bus: is there a bench, and does it have this bus? */
    char names[ZB_MAX * 2];
    uint32_t pid = 0;
    if (z_i2cx_bench_buses(names, sizeof(names)) < 0) return Z_I2CX_NO_BENCH;
    bool found = false;
    for (char *p = names; *p && !found; p += strlen(p) + 1) found = !strcmp(p, name);
    if (!found) return Z_I2CX_NO_BUS;
    if (!z_pid_lookup(ZB_PROVIDER, &pid) || !pid ||
        z_port_connect_arg(&b->port, pid, z_obj_str(ZB_TAG)) != Z_OK)
        return Z_I2CX_NO_BENCH;
    b->bench = true;
    return Z_I2C_OK;
}

void z_i2cx_close(z_i2cx_t *b) {
    if (b->bench && b->port.connected) z_port_close(&b->port);
    b->bench = false;
}

static int bench_xfer(z_i2cx_t *b, uint8_t addr, const uint8_t *w, uint32_t wn,
                      uint8_t *r, uint32_t rn) {
    uint8_t req[ZB_NAME + ZB_MAX + 8], rep[ZB_MAX + 2];
    int n = zb_xfer_encode(req, b->name, addr, w, (int)wn, (int)rn);
    if (n < 0) return Z_I2C_NACK;
    n = ask(b, req, n, rep, sizeof(rep));
    if (n < 2) return Z_I2C_TIMEOUT;
    if (rep[0] != ZB_ACK) return Z_I2C_NACK;
    if (rn) memcpy(r, rep + 2, rn);
    return Z_I2C_OK;
}

int z_i2cx_write(z_i2cx_t *b, uint8_t addr, const uint8_t *d, uint32_t n) {
    if (b->bench) return bench_xfer(b, addr, d, n, 0, 0);
    return z_i2c_write(&b->pins, addr, d, n, true);
}

int z_i2cx_read(z_i2cx_t *b, uint8_t addr, uint8_t *d, uint32_t n) {
    if (b->bench) return bench_xfer(b, addr, 0, 0, d, n);
    return z_i2c_read(&b->pins, addr, d, n, true);
}

int z_i2cx_write_read(z_i2cx_t *b, uint8_t addr, const uint8_t *w, uint32_t wn,
                      uint8_t *r, uint32_t rn) {
    if (b->bench) return bench_xfer(b, addr, w, wn, r, rn);
    return z_i2c_write_read(&b->pins, addr, w, wn, r, rn);
}

int z_i2cx_probe(z_i2cx_t *b, uint8_t addr) {
    if (b->bench) return bench_xfer(b, addr, 0, 0, 0, 0);
    return z_i2c_probe(&b->pins, addr) ? Z_I2C_OK : Z_I2C_NACK;
}
