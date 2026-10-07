/*
 * sechs -- host tests for the commands (sechs_cmd.c) against a fake
 * module on a fake bus: its registers, INFO, an I2C console that echoes
 * and answers, CONTROL, ADDR and REG. `make test` in sw/apps/sechs.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../sechs_cmd.h"

static int failures;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %d: ", __LINE__); \
    printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

/* ---- the fake module ---- */

static uint8_t m_addr = 0x0C, m_caps = 0x07, m_ok = 0x1F, m_cmd, m_regs[16];
static char m_in[128], m_out[512];
static int m_in_len, m_out_len;
static const char m_info[] = "fw=Fake\nmod=TEST\n";

static void m_say(const char *s) {
    while (*s && m_out_len < (int)sizeof(m_out)) m_out[m_out_len++] = *s++;
}

static void m_line(void) {
    m_in[m_in_len] = 0;
    m_say("\r\n");
    if (!strcmp(m_in, "PRINT 2+2")) m_say("4\r\n");
    if (!strcmp(m_in, "FAIL")) {
        m_say("SYNTAX ERROR\r\n");
        m_ok &= ~0x10;
    } else {
        m_ok |= 0x10;
    }
    m_in_len = 0;
}

static int f_write(void *c, uint8_t addr, const uint8_t *d, int n) {
    (void)c;
    if (addr != m_addr || n < 1) return -1;
    uint8_t reg = d[0];
    if (reg == SR_CDATA) {
        for (int i = 1; i < n; i++) {
            char ch[2] = { (char)d[i], 0 };
            if (d[i] == '\r') m_line();
            else {
                m_say(ch);                      /* the console echoes */
                if (m_in_len < 127) m_in[m_in_len++] = (char)d[i];
            }
        }
    } else if (reg == SR_CONTROL && n == 2) {
        m_cmd = d[1];
    } else if (reg == SR_ADDR && n == 3 && d[2] == (uint8_t)~d[1]) {
        m_addr = d[1];
    } else if (reg >= SR_REG && reg < SR_REG + 16 && n == 2) {
        m_regs[reg - SR_REG] = d[1];
    }
    return 0;
}

static int f_read(void *c, uint8_t addr, uint8_t reg, uint8_t *d, int n) {
    (void)c;
    if (addr != m_addr) return -1;
    if (reg == SR_INFO) {
        memset(d, 0, n);
        memcpy(d, m_info, sizeof(m_info) < (size_t)n ? sizeof(m_info) : (size_t)n);
        return 0;
    }
    if (reg == SR_CDATA) {
        int k = n < m_out_len ? n : m_out_len;
        memcpy(d, m_out, k);
        memmove(m_out, m_out + k, m_out_len - k);
        m_out_len -= k;
        return 0;
    }
    for (int i = 0; i < n; i++, reg++) {
        uint8_t v = 0;
        switch (reg) {
        case SR_SIG0: v = 'S'; break;
        case SR_SIG1: v = '6'; break;
        case SR_VER: v = 0x05; break;
        case SR_CAPS: v = m_caps; break;
        case SR_STATUS: v = 0x14; break;            /* running networked */
        case SR_OK: v = m_ok; break;
        case SR_FAULT: v = 0; break;
        case SR_CIN: v = 32; break;
        case SR_COUT: v = (uint8_t)(m_out_len < 255 ? m_out_len : 255); break;
        default:
            if (reg >= SR_REG && reg < SR_REG + 16) v = m_regs[reg - SR_REG];
        }
        d[i] = v;
    }
    return 0;
}

static long clock_ms;
static void f_idle(void *c) { (void)c; clock_ms += 2; }
static long f_ms(void *c) { (void)c; return clock_ms; }

/* ---- the terminal ---- */

static char out[4096];
static int out_len;
static const char *keys;

static void f_print(const char *s) {
    while (*s && out_len < (int)sizeof(out) - 1) out[out_len++] = *s++;
    out[out_len] = 0;
}

static void f_out(void *c, char ch) {
    char s[2] = { ch, 0 };
    (void)c;
    f_print(s);
}

/* a key now and then, with "no key yet" in between */
static int f_key(void) {
    static int toggle;
    if (!keys || !*keys) return -2;
    if ((toggle ^= 1)) return -1;
    return (uint8_t)*keys++;
}

static char *f_read_file(const char *path) {
    if (strcmp(path, "PROG.BAS")) return 0;
    return strdup("PRINT 2+2\nFAIL\n");
}

static char *f_read_ok(const char *path) {
    (void)path;
    return strdup("PRINT 2+2\r\n");
}

static void f_free(char *d) { free(d); }

static sx_io_t io = {
    { f_write, f_read, f_idle, f_ms, f_out, 0 }, f_print, f_key, f_read_file, f_free
};

static int run(const char *line) {
    char buf[256], *argv[8];
    int argc = 0;
    out_len = 0;
    out[0] = 0;
    strcpy(buf, line);
    for (char *p = strtok(buf, " "); p && argc < 8; p = strtok(0, " ")) argv[argc++] = p;
    return sx_run(&io, argc, argv);
}

int main(void) {
    CHECK(run("scan") == 0 && !strcmp(out, "0x0c running networked\n"), "scan: [%s]", out);
    CHECK(run("info 0x0c") == 0 && !strcmp(out,
        "address 0x0c\nversion 0.5\ncaps    0x07\nstatus  running networked\n"
        "ok      0x1f\nfault   0\nfw=Fake\nmod=TEST\n"), "info: [%s]", out);
    CHECK(run("info 12") == 0, "decimal addresses");
    CHECK(run("info 0x10") == 1 && !strcmp(out, "no Sechs module at 0x10\n"), "no module: [%s]", out);
    CHECK(run("info 0x80") == 2 && strstr(out, "0x08-0x77"), "address out of range");

    CHECK(run("reg 0x0c 3 42") == 0 && m_regs[3] == 42, "reg write");
    CHECK(run("reg 0x0c 3") == 0 && !strcmp(out, "42\n"), "reg read: [%s]", out);
    CHECK(run("reg 0x0c 16") == 2, "reg 16 refused");

    CHECK(run("halt 0x0c") == 0 && m_cmd == CMD_HALT, "halt");
    CHECK(run("run 0x0c") == 0 && m_cmd == CMD_RUN, "run");
    CHECK(run("reset 0x0c") == 0 && m_cmd == CMD_RESET, "reset");
    m_cmd = 0;
    CHECK(run("program 0x0c") == 1 && m_cmd == 0 && strstr(out, "no programming mode"),
          "program refused without CAPS bit 7: [%s]", out);
    m_caps |= 0x80;
    CHECK(run("program 0x0c") == 0 && m_cmd == CMD_PROGRAM, "program with CAPS bit 7");

    CHECK(run("addr 0x0c 0x22") == 0 && m_addr == 0x22, "addr moves the module");
    CHECK(run("scan") == 0 && !strncmp(out, "0x22", 4), "found at the new address");
    CHECK(run("addr 0x22 0x0c") == 0 && m_addr == 0x0C, "and back");
    CHECK(run("addr 0x0c 0x78") == 2, "a new address out of range");

    /* send: lines into the console; the last command decides */
    CHECK(run("send 0x0c PROG.BAS") == 1 && strstr(out, "PRINT 2+2\n4\n") &&
          strstr(out, "SYNTAX ERROR") && strstr(out, "the last command failed"),
          "send, ending with a failure: [%s]", out);
    io.read_file = f_read_ok;
    CHECK(run("send 0x0c X.BAS") == 0 && strstr(out, "PRINT 2+2\n4\n"), "send: [%s]", out);
    io.read_file = f_read_file;
    CHECK(run("send 0x0c NONE.BAS") == 2 && strstr(out, "cannot read"), "send: no file");

    /* the console: typed with a mistake corrected, then Ctrl-D */
    keys = "PRINT 2+3\x7f" "2\r\x04";
    CHECK(run("console 0x0c") == 0, "console");
    CHECK(strstr(out, "PRINT 2+3\b \b2\n") && strstr(out, "4\n"),
          "console: local echo, backspace, the answer: [%s]", out);
    keys = "\x1b[AHELLO\r";                     /* an arrow key: skipped */
    run("console 0x0c");
    CHECK(strstr(out, "HELLO\n") && !strstr(out, "[A"), "escape sequences skipped: [%s]", out);
    keys = "X\r";
    m_addr = 0x30;                              /* the module goes away */
    CHECK(run("console 0x0c") == 1 && strstr(out, "stopped answering"), "a module gone: [%s]", out);
    m_addr = 0x0C;

    CHECK(run("bogus 0x0c") == 2 && strstr(out, "usage:"), "usage");
    CHECK(run("") == 2, "no command");

    if (failures) {
        printf("FAILED: %d\n", failures);
        return 1;
    }
    printf("all sechs tests passed\n");
    return 0;
}
