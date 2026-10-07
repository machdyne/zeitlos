#ifndef SECHS_CMD_H
#define SECHS_CMD_H

/*
 * sechs -- the commands, independent of Zeitlos: sechs_app.c gives them a
 * bus (zi2c on a PMOD) and a terminal; tests/test_sechs.c a simulated
 * module. The protocol is the Sechs master library (sw/ext/basic/tools/
 * sechs/sechsm.c), the same code as Linux's sechsctl, and the output is
 * sechsctl's.
 */

#include "../../ext/basic/tools/sechs/sechsm.h"

typedef struct {
    sm_bus bus;                         /* its out() goes to print_c() */
    void (*print)(const char *s);       /* text, with \n line ends */
    /* console: the next key from the terminal, -1 if none yet, -2 if
     * the terminal is gone */
    int (*key)(void);
    /* send: the whole file, NUL-terminated (the caller frees it), or 0 */
    char *(*read_file)(const char *path);
    void (*free_file)(char *data);
} sx_io_t;

/* argv[0] is the command (scan, info, ...). Returns the exit status:
 * 0, 1 if the module did not do it, 2 for a usage error. */
int sx_run(sx_io_t *io, int argc, char **argv);

void sx_usage(sx_io_t *io);

#endif
