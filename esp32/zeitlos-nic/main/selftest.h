#ifndef SELFTEST_H
#define SELFTEST_H

/* UART0 CLI (passthru / US1). No SD, no Zeitlos. See docs/esp32-selftest.md. */
void selftest_start(void);

int nic_start_sta(const char *ssid, const char *psk);	/* main.c */
#endif
