/*
 * zapi (sw/common/zapi) on the build machine, for slide's host tests:
 * the Zeitlos API needs the board, so script.c's few calls into it do
 * nothing here.
 */

#include <stdbool.h>
#include "../../../common/zmsg.h"

void zapi_register(void) {}
bool zapi_win_msg(z_msg_t *m) { (void)m; return false; }
int zapi_win_event(z_msg_t *m, char *e, int c, int *conn) { (void)m; (void)e; (void)c; (void)conn; return 0; }
void zapi_win_event_done(void) {}
bool zapi_win_owns(int id) { (void)id; return false; }
int repl_cur_conn = -1;
