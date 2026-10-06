/*
 * netserve.c in its own translation unit, for tests/test_e2e.c: its
 * main loop, one pass at a time. (Its statics would collide with
 * tcp.c's if both were included into one file.)
 */
#define main netserve_main_unused
#include "../netserve.c"
#undef main

void ns_init(void) {
	memset(sess, 0, sizeof(sess));
	read_config();
}

// One pass of main()'s loop, minus the wait.
void ns_step(void) {
	z_msg_t m;
	net_replaced(z_uptime_ticks());
	if (net_pid || z_pid_lookup("net0", &net_pid)) {
		if (svc_telnet[0].on && !svc_telnet[0].listening) listen_on(svc_telnet[0].port);
		if (svc_echo.on && !svc_echo.listening) listen_on(svc_echo.port);
	}
	while (z_msg_read(&m) == Z_OK) on_msg(&m);
	for (int i = 0; i < MAX_SESS; i++) if (sess[i].state) poll_session(&sess[i]);
}
