/*
 * i2c -- I2C from the posix shell, on a PMOD port or a bench bus
 * (docs/i2c_app.md):
 *
 *   i2c [-b BUS] buses                       the buses there are
 *   i2c [-b BUS] scan                        the addresses that answer
 *   i2c [-b BUS] read ADDR N                 N bytes from ADDR
 *   i2c [-b BUS] write ADDR BYTE...          bytes to ADDR
 *   i2c [-b BUS] write ADDR BYTE... read N   then N back, repeated start
 *
 * BUS is pmod0 (the default) ... pmodN, or a bench bus (zi2cx.h).
 * Numbers are decimal, or hex with 0x. Exit status: 0; 1 if the device
 * did not answer; 2 for a usage error or a bus that is not there.
 *
 * Output goes to the shell's terminal through posix's "stdout" relay,
 * as zcc's does. No printf: it would link newlib's formatter, about
 * 100KB, for a few hex numbers.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zport.h"
#include "../../common/zwin.h"          /* z_launch_arg_take() */
#include "../../common/zargs.h"
#include "../../common/zi2cx.h"
#include "../posix/posix.h"

void uart_putc(char c);

static z_port_t out;
static bool out_on;

static void put(const char *s) {
    char buf[256];
    int n = 0;
    for (; *s; s++) {
        if (n > (int)sizeof(buf) - 3) {
            if (out_on) z_port_send(&out, buf, (uint32_t)n);
            else for (int i = 0; i < n; i++) uart_putc(buf[i]);
            n = 0;
        }
        if (*s == '\n') buf[n++] = '\r';
        buf[n++] = *s;
    }
    if (!n) return;
    if (out_on) {
        for (int i = 0; i < 32 && z_port_send(&out, buf, (uint32_t)n) != Z_OK; i++) {
            z_msg_t m;
            while (z_msg_read(&m) == Z_OK)
                if (m.subject == Z_PORT_DATA_ACK) z_port_handle_ack(&out, &m);
            z_proc_wait(1);
        }
    } else {
        for (int i = 0; i < n; i++) uart_putc(buf[i]);
    }
}

static void hex2(unsigned v, char *b) {
    b[0] = "0123456789abcdef"[(v >> 4) & 15];
    b[1] = "0123456789abcdef"[v & 15];
    b[2] = 0;
}

static long num(const char *s) {
    long v = 0;
    int base = 10;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    }
    if (!*s) return -1;
    for (; *s; s++) {
        int d = *s >= '0' && *s <= '9' ? *s - '0' :
            base == 16 && *s >= 'a' && *s <= 'f' ? *s - 'a' + 10 :
            base == 16 && *s >= 'A' && *s <= 'F' ? *s - 'A' + 10 : -1;
        if (d < 0 || v > 0xFFFF) return -1;
        v = v * base + d;
    }
    return v;
}

static int usage(void) {
    put("usage: i2c [-b BUS] buses | scan | read ADDR N |\n"
        "           write ADDR BYTE... [read N]\n"
        "  BUS: pmod0 (default) ... pmodN, or a bench bus\n");
    return 2;
}

static void bytes(const uint8_t *d, int n) {
    char b[3];
    for (int i = 0; i < n; i++) {
        hex2(d[i], b);
        put(b);
        put(i + 1 < n ? " " : "\n");
    }
}

static int run(int argc, char **argv) {
    const char *busname = "pmod0";
    z_i2cx_t bus;
    int a = 0, r;
    if (argc >= 2 && !strcmp(argv[0], "-b")) {
        busname = argv[1];
        a = 2;
    }
    if (argc - a < 1) return usage();
    const char *cmd = argv[a++];

    if (!strcmp(cmd, "buses")) {
        char names[256];
        int n = z_i2cx_bench_buses(names, sizeof(names));
        put("pmod0 ... (PMOD ports)\n");
        if (n < 0) put("no bench running\n");
        for (char *p = names; n > 0 && *p; p += strlen(p) + 1) {
            put(p);
            put(" (bench)\n");
        }
        return 0;
    }

    r = z_i2cx_open(&bus, busname);
    if (r == Z_I2CX_NO_BENCH) {
        put(busname);
        put(": no bench running (or no such PMOD port)\n");
        return 2;
    }
    if (r == Z_I2CX_NO_BUS) {
        put(busname);
        put(": no such bus\n");
        return 2;
    }
    if (r != Z_I2C_OK) {
        put(busname);
        put(": the bus is stuck (no pull-ups, or a device holding a line)\n");
        return 2;
    }

    if (!strcmp(cmd, "scan")) {
        int found = 0;
        char b[3];
        for (int addr = 0x08; addr <= 0x77; addr++)
            if (z_i2cx_probe(&bus, (uint8_t)addr) == Z_I2C_OK) {
                hex2((unsigned)addr, b);
                put("0x");
                put(b);
                put("\n");
                found++;
            }
        if (!found) put("nothing answers\n");
        z_i2cx_close(&bus);
        return 0;
    }

    if (argc - a < 1) return usage();
    long addr = num(argv[a++]);
    if (addr < 0x08 || addr > 0x77) {
        put("addresses are 0x08-0x77\n");
        return 2;
    }

    uint8_t w[64], rd[64];
    int wn = 0, rn = 0;
    if (!strcmp(cmd, "read")) {
        if (argc - a != 1 || (rn = (int)num(argv[a])) < 1 || rn > 64) return usage();
        r = z_i2cx_read(&bus, (uint8_t)addr, rd, (uint32_t)rn);
    } else if (!strcmp(cmd, "write")) {
        for (; a < argc && strcmp(argv[a], "read"); a++) {
            long v = num(argv[a]);
            if (v < 0 || v > 255 || wn >= 64) return usage();
            w[wn++] = (uint8_t)v;
        }
        if (a < argc) {                         /* read N */
            if (argc - a != 2 || (rn = (int)num(argv[a + 1])) < 1 || rn > 64) return usage();
        }
        if (!wn) return usage();
        r = rn ? z_i2cx_write_read(&bus, (uint8_t)addr, w, (uint32_t)wn, rd, (uint32_t)rn)
               : z_i2cx_write(&bus, (uint8_t)addr, w, (uint32_t)wn);
    } else {
        return usage();
    }
    z_i2cx_close(&bus);
    if (r != Z_I2C_OK) {
        char b[3];
        hex2((unsigned)addr, b);
        put("0x");
        put(b);
        put(r == Z_I2C_NACK ? ": no answer (NACK)\n" : ": the bus is stuck\n");
        return 1;
    }
    if (rn) bytes(rd, rn);
    return 0;
}

int main(void) {
    static char line[256], buf[256];
    char *argv[40];
    uint8_t flags[40];
    int argc = 0;
    uint32_t pid = 0;
    if (z_launch_arg_take(line, sizeof(line)) && line[0])
        argc = z_args_split(line, buf, sizeof(buf), argv, flags, 40);
    if (z_pid_lookup("posix0", &pid) && pid &&
        z_port_connect_arg(&out, pid, z_obj_str(PX_STDOUT_TAG)) == Z_OK)
        out_on = true;
    int r = run(argc, argv);
    /* read before this process, and the heap the output sits on, goes
     * (z_port_drain(), zport.h) */
    if (out_on) {
        z_port_drain(&out, Z_TICK_HZ * 2);
        z_port_close(&out);
    }
    return r;
}
