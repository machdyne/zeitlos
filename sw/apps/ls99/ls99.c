/*
 * ls99 -- a virtual Zwölf module (ls99.h). Each piece below follows its
 * counterpart in upstream's targets/ls1x/module.c, which runs on real
 * LS10 and LS11 modules; where it must differ, it says why.
 */

#include <string.h>
#include "ls99.h"
#include "../../ext/basic/basic.h"
#include "../../ext/basic/sechs/sechs.h"

static const ls99_board_t *board;
static int profile;
static uint8_t halted;
static uint8_t cmd_len;
static uint8_t mode[LS99_PINS];             /* PM_* of each pin */
static bool reset_asked;

static const char *const info_text[] = {
    "fw=Machdyne BASIC\nmod=LS99\nprofile=LS10\nlang=basic\n",
    "fw=Machdyne BASIC\nmod=LS99\nprofile=LS11\nlang=basic\n",
    "fw=Machdyne BASIC\nmod=LS99\nprofile=LS99\nlang=basic\n",
};

void ls99_init(const ls99_board_t *b, int prof, uint8_t addr) {
    board = b;
    profile = prof;
    halted = 0;
    cmd_len = 0;
    reset_asked = false;
    memset(mode, 0, sizeof(mode));
    /* the machine played: its program area and its pins */
    basic_prog_max = prof == LS99_LS10 ? 1024 : prof == LS99_LS11 ? 4096 : 32768;
    basic_pins = prof == LS99_LS10 ? 4 : 7;
    for (int i = 0; i < LS99_PINS; i++) b->pin_drive(i, LS99_NONE);
    sechs_init(addr, CAP_FILES | CAP_I2C_CON);
}

uint8_t ls99_addr(void) {
    return sechs.addr;
}

bool ls99_halted(void) {
    return halted;
}

/* ---- the Sechs core's side (sechs.h) ---- */

/* The console's output is full: on a module, the master reads it during
 * these 2ms. Here the master's transactions arrive as the board's
 * messages, so the wait is the board's turn. */
void sechs_wait(void) {
    board->service();
}

void sechs_set_addr(uint8_t addr) {
    board->set_addr(addr);
}

uint8_t sechs_info(uint8_t i) {
    const char *t = info_text[profile];
    return i < strlen(t) ? (uint8_t)t[i] : 0;
}

uint8_t *sechs_regs(void) {
    return basic_regs;
}

void ls99_slave(const uint8_t *w, int wn, uint8_t *r, int rn) {
    if (wn) {
        sechs_start(0);
        for (int i = 0; i < wn; i++) sechs_rx(w[i]);
        if (!rn) {
            sechs_stop(0);
            return;
        }
    }
    sechs_start(0);
    /* like the hardware: one byte is always loaded ahead, and the last
     * one loaded is never sent */
    for (int i = 0; i < rn; i++) r[i] = sechs_tx();
    sechs_tx();
    sechs_stop(1);
}

/* ---- the main loop's work (module.c's service and console_char) ---- */

static void service(void) {
    uint8_t cmd = sechs.cmd;
    uint8_t fault = basic_prog_err == BASIC_E_BUS ? FAULT_BUS :
        basic_prog_err && basic_prog_err != BASIC_E_BREAK ? FAULT_PROGRAM : 0;
    sechs.cmd = 0;
    sechs.r[SR_FAULT] = fault;
    sechs.r[SR_STATUS] = (sechs.r[SR_STATUS] & ST_BOOT) |
        (sechs.networked ? ST_NETWORKED : 0) | (halted ? ST_HALTED : 0) |
        (basic_running ? ST_RUNNING : 0) | (fault ? ST_FAULT : 0) |
        (sechs.con_active ? ST_CONSOLE : 0);
    uint8_t s = sechs.r[SR_STATUS];
    sechs.r[SR_OK] = ~(((s >> 4) & 0x06) | ((s << 2) & 0x08) |
        (basic_cmd_err ? 0x10 : 0)) & 0x1F;
    if (cmd == CMD_RESET) reset_asked = true;   /* the board starts it again */
    if (cmd == CMD_HALT) halted = 1;
    if (cmd == CMD_RUN) {
        halted = 0;
        if (!basic_running) basic_yield((uint8_t *)"RUN");
    }
}

static void console_char(uint8_t c) {
    hw_putc((char)c);                           /* echo */
    if (c == '\r' || c == '\n') {
        if (c == '\r') hw_putc('\n');
        basic_line[cmd_len] = '\0';
        cmd_len = 0;
        basic_yield((uint8_t *)basic_line);
        service();
    } else if (cmd_len < BASIC_LINE - 1) {
        basic_line[cmd_len++] = (char)c;
    }
}

void ls99_line(const char *s) {
    while (*s) console_char((uint8_t)*s++);
    console_char('\r');
}

bool ls99_poll(void) {
    int c;
    board->service();
    service();
    while ((c = sechs_getc()) >= 0) console_char((uint8_t)c);
    bool r = reset_asked;
    reset_asked = false;
    return r;
}

/* ---- what BASIC asks of a module (basic.h) ---- */

void hw_putc(char c) {
    board->console(c);
    if (sechs.con_active) sechs_putc(c);
}

/* HALT or Ctrl-C on the I2C console stop a program, as on a module */
int hw_break(void) {
    board->service();
    if (sechs.cmd == CMD_HALT || sechs.con_break) {
        sechs.con_break = 0;
        service();
        return 1;
    }
    if (sechs.cmd) service();
    return 0;
}

void hw_delay_ms(uint16_t ms) {
    board->sleep(ms);
}

/* module.c's rules: no UART yet; on A and B (the Sechs bus) an output is
 * refused once a master has addressed the module, anything else is fine */
int hw_pin_mode(uint8_t pin, uint8_t m) {
    int i = pin - 1;
    if (i < 0 || i >= basic_pins) return HW_ERR_UNSUPPORTED;
    if (m == PM_UART) return HW_ERR_UNSUPPORTED;
    if (i < 2 && (m == PM_OD || m == PM_PP) && sechs.networked) return HW_ERR_BUS;
    mode[i] = m;
    /* an output starts low (OD: pulling down), as the hardware's reset
     * state of the output register; everything else lets go */
    board->pin_drive(i, m == PM_PP || m == PM_OD ? LS99_LOW : LS99_NONE);
    return 0;
}

void hw_pin_write(uint8_t pin, uint8_t level) {
    int i = pin - 1;
    if (mode[i] == PM_PP) board->pin_drive(i, level ? LS99_HIGH : LS99_LOW);
    else if (mode[i] == PM_OD) board->pin_drive(i, level ? LS99_NONE : LS99_LOW);
}

uint8_t hw_pin_read(uint8_t pin) {
    return board->pin_level(pin - 1) != LS99_L0;    /* floating reads 1 */
}

/* a net is digital: 0, 1023, and halfway when nothing drives it */
int16_t hw_adc(uint8_t pin) {
    int lv = board->pin_level(pin - 1);
    return lv == LS99_L1 ? 1023 : lv == LS99_L0 ? 0 : 512;
}

void hw_led(uint8_t on) {
    board->pin_drive(LS99_LED, on ? LS99_HIGH : LS99_LOW);
}

int hw_i2c(uint8_t addr, const uint8_t *w, uint8_t wn, uint8_t *r, uint8_t rn) {
    return board->i2c(addr, w, wn, r, rn);
}
