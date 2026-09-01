#ifndef ESP32LINK_H
#define ESP32LINK_H

#include <stdint.h>
#include <stdbool.h>

bool     esp32link_init(const uint8_t mac[6]);
uint16_t esp32link_recv(uint8_t *buf, uint16_t maxlen);
bool     esp32link_send(const uint8_t *buf, uint16_t len);
void     esp32link_debug_dump(void);
/* Non-blocking bring-up for the main loop (do not block wm). */
void     esp32link_poll_wifi(const char *ssid, const char *psk);
int      esp32link_hello_ok(void);
int      esp32link_link_is_up(void);

#endif
