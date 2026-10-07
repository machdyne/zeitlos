#ifndef LS99_H
#define LS99_H

/*
 * ls99 -- a virtual Zwölf module (docs/ls99.md): Machdyne BASIC and the
 * Sechs module core, as on an LS10 or LS11, with the board around them
 * made of messages to bench. One module per process, as one program per
 * chip.
 *
 * This is the module, portable: it is told about its board through
 * ls99_board_t. ls99_app.c is the board on Zeitlos (bench, reached
 * through zbench.h); tests/ make one out of bench's own core, in one
 * process, with exact virtual time.
 *
 * What it does is what targets/ls1x/module.c does on a real module, and
 * in the same order: the console, CONTROL (HALT, RUN, RESET), STATUS,
 * OK and FAULT, the pin rules (A and B are the Sechs bus), Ctrl-C.
 */

#include <stdint.h>
#include <stdbool.h>

/* the machine played (BASIC_PROFILE, sw/ext/basic/basic.h) */
#define LS99_LS10   0           /* 1KB programs, pins A-D */
#define LS99_LS11   1           /* 4KB, pins A-G */
#define LS99_LS99   2           /* 32KB, pins A-G: no machine's limits */

/* pins, in the order bench knows them: A-G, then the LED */
#define LS99_PINS   8
#define LS99_LED    7

/* levels, as bench resolves them (the same numbers as bench's core) */
#define LS99_L0     0
#define LS99_L1     1
#define LS99_FLOAT  2

/* drives, as bench takes them */
#define LS99_NONE   0
#define LS99_LOW    1
#define LS99_HIGH   2

typedef struct {
    void (*pin_drive)(int pin, int drive);
    int (*pin_level)(int pin);
    /* the module's local bus (C, D): 0 if ACKed, -1 if not, or no bus */
    int (*i2c)(uint8_t addr, const uint8_t *w, int wn, uint8_t *r, int rn);
    void (*sleep)(uint32_t ms);         /* virtual time passes */
    void (*service)(void);              /* the board's messages: Sechs
                                           transactions, levels */
    void (*console)(char c);            /* a copy of what it prints */
    void (*set_addr)(uint8_t addr);     /* its Sechs address changed */
} ls99_board_t;

void ls99_init(const ls99_board_t *b, int profile, uint8_t addr);

/* a line typed at its console (as through the Sechs console) */
void ls99_line(const char *s);

/* the main loop's turn: CONTROL commands, console input. Returns true if
 * a RESET was asked for: the board starts it again. */
bool ls99_poll(void);

/* A transaction addressed to it on its Sechs bus, replayed into the core
 * the way the hardware does (one byte read ahead, never sent). */
void ls99_slave(const uint8_t *w, int wn, uint8_t *r, int rn);

uint8_t ls99_addr(void);
bool ls99_halted(void);

#endif
