/*
 * sechs -- the commands (sechs_cmd.h). No printf: one would link
 * newlib's formatter, about 100KB (docs/app_runtime.md); the few numbers
 * here are formatted by hand.
 */

#include <string.h>
#include "sechs_cmd.h"

static sx_io_t *io;

static void put(const char *s) {
    io->print(s);
}

static void hex2(unsigned v) {
    char b[5] = { '0', 'x', "0123456789abcdef"[(v >> 4) & 15], "0123456789abcdef"[v & 15], 0 };
    put(b);
}

static void dec(long v) {
    char b[16];
    int i = sizeof(b) - 1;
    int neg = v < 0;
    unsigned long u = neg ? -(unsigned long)v : (unsigned long)v;
    b[i] = 0;
    do b[--i] = (char)('0' + u % 10); while (u /= 10);
    if (neg) b[--i] = '-';
    put(b + i);
}

/* a number: decimal, or hex with 0x; -1 if it is not one */
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
        if (d < 0) return -1;
        v = v * base + d;
        if (v > 0xFFFF) return -1;
    }
    return v;
}

static void status(int s) {
    for (int i = 0; i < 7; i++)
        if (s & (1 << i)) {
            put(" ");
            put(sm_status_names[i]);
        }
}

void sx_usage(sx_io_t *x) {
    io = x;
    put("usage: sechs [-p PORT | -b BUS] scan | info ADDR | reg ADDR N [VALUE] |\n"
        "             halt|run|reset|program ADDR | addr ADDR NEW |\n"
        "             console ADDR | send ADDR FILE\n"
        "  -p PORT: PMOD port PORT (default 0), pin 1 (A) SCL, pin 2 (B) SDA\n"
        "  -b BUS: a bus by name: pmodN, or a bench bus (virtual modules)\n");
}

static int scan(void) {
    uint8_t found[112];
    int n = sm_scan(&io->bus, found, sizeof(found));
    for (int k = 0; k < n; k++) {
        hex2(found[k]);
        status(sm_reg(&io->bus, found[k], SR_STATUS));
        put("\n");
    }
    if (!n) put("no modules found\n");
    return 0;
}

static int info(uint8_t addr) {
    sm_info_t i;
    if (sm_info(&io->bus, addr, &i)) {
        put("no Sechs module at ");
        hex2(addr);
        put("\n");
        return 1;
    }
    put("address ");
    hex2(addr);
    put("\nversion ");
    dec(i.version >> 4);
    put(".");
    dec(i.version & 15);
    put("\ncaps    ");
    hex2(i.caps);
    put("\nstatus ");
    status(i.status);
    put("\nok      ");
    hex2(i.ok);
    put("\nfault   ");
    dec(i.fault);
    put("\n");
    put(i.info);
    return 0;
}

/* the I2C console, interactively: a line is edited here (the terminal
 * sends keys), then typed into the module; its output is shown as it
 * arrives, also while nothing is typed */
static int console(uint8_t addr) {
    char line[128];
    int len = 0, esc = 0;
    put("console on ");
    hex2(addr);
    put("; end with Ctrl-D\n");
    for (;;) {
        int k = io->key();
        if (k == -2 || k == 0x04 || k == 0x03) break;
        if (k < 0) {
            if (sm_drain(&io->bus, addr) < 0) goto gone;
            io->bus.idle(io->bus.ctx);
            continue;
        }
        /* escape sequences (arrows and the like): skipped */
        if (esc) {
            if (esc == 1 && k == '[') esc = 2;
            else if (esc == 2 && k >= '0' && k <= '9') esc = 2;
            else esc = 0;
            continue;
        }
        if (k == 0x1B) {
            esc = 1;
            continue;
        }
        if (k == '\r' || k == '\n') {
            put("\n");
            if (sm_line(&io->bus, addr, line, len)) goto gone;
            len = 0;
        } else if (k == 0x7F || k == '\b') {
            if (len) {
                len--;
                put("\b \b");
            }
        } else if (k >= 0x20 && len < (int)sizeof(line) - 1) {
            char c[2] = { (char)k, 0 };
            line[len++] = (char)k;
            put(c);
        }
    }
    put("\n");
    return 0;
gone:
    put("the module at ");
    hex2(addr);
    put(" stopped answering\n");
    return 1;
}

/* a file's lines typed into the console, one at a time */
static int send(uint8_t addr, const char *path) {
    char *data = io->read_file(path);
    int r = 0;
    if (!data) {
        put(path);
        put(": cannot read it\n");
        return 2;
    }
    for (char *p = data; *p && !r; ) {
        int n = (int)strcspn(p, "\r\n");
        if (sm_line(&io->bus, addr, p, n)) {
            put("the module at ");
            hex2(addr);
            put(" stopped answering\n");
            r = 2;
        }
        p += n;
        if (*p == '\r') p++;
        if (*p == '\n') p++;
    }
    io->free_file(data);
    if (!r && sm_last_ok(&io->bus, addr) != 1) {
        put("the last command failed\n");
        r = 1;
    }
    return r;
}

int sx_run(sx_io_t *x, int argc, char **argv) {
    io = x;
    if (argc < 1) goto usage;
    const char *cmd = argv[0];
    if (!strcmp(cmd, "scan")) return scan();
    if (argc < 2) goto usage;
    long a = num(argv[1]);
    if (a < 0x08 || a > 0x77) {
        put("addresses are 0x08-0x77\n");
        return 2;
    }
    uint8_t addr = (uint8_t)a;
    if (!strcmp(cmd, "info")) return info(addr);
    if (!strcmp(cmd, "halt") || !strcmp(cmd, "run") || !strcmp(cmd, "reset") ||
        !strcmp(cmd, "program")) {
        uint8_t c = cmd[0] == 'h' ? CMD_HALT : cmd[0] == 'p' ? CMD_PROGRAM :
            cmd[1] == 'u' ? CMD_RUN : CMD_RESET;
        int r = sm_control(&io->bus, addr, c);
        if (r == -2) {
            put("the module at ");
            hex2(addr);
            put(" has no programming mode\n");
        } else if (r) {
            put("no answer from ");
            hex2(addr);
            put("\n");
        }
        return r ? 1 : 0;
    }
    if (!strcmp(cmd, "addr") && argc == 3) {
        long n = num(argv[2]);
        if (n < 0x08 || n > 0x77) {
            put("addresses are 0x08-0x77\n");
            return 2;
        }
        if (sm_set_address(&io->bus, addr, (uint8_t)n)) {
            put("address not changed\n");
            return 1;
        }
        return 0;
    }
    if (!strcmp(cmd, "reg") && (argc == 3 || argc == 4)) {
        long n = num(argv[2]);
        if (n < 0 || n > 15) goto usage;
        if (argc == 4) {
            long v = num(argv[3]);
            if (v < 0 || v > 255) goto usage;
            uint8_t b = (uint8_t)v;
            return sm_reg_write(&io->bus, addr, SR_REG + n, &b, 1) ? 1 : 0;
        }
        int v = sm_reg(&io->bus, addr, SR_REG + n);
        if (v < 0) return 1;
        dec(v);
        put("\n");
        return 0;
    }
    if (!strcmp(cmd, "console") && argc == 2) return console(addr);
    if (!strcmp(cmd, "send") && argc == 3) return send(addr, argv[2]);
usage:
    sx_usage(x);
    return 2;
}
