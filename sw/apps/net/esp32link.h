#ifndef ESP32LINK_H
#define ESP32LINK_H

#include "netcfg.h"

#include <stdint.h>
#include <stdbool.h>

bool     esp32link_init(const uint8_t mac[6]);
uint16_t esp32link_recv(uint8_t *buf, uint16_t maxlen);
bool     esp32link_send(const uint8_t *buf, uint16_t len);
void     esp32link_debug_dump(void);
/* Non-blocking bring-up for the main loop (do not block wm). */
void     esp32link_poll_wifi(const netcfg_t *cfg);
/* Ticks until this link wants net's loop again (0 = now), so the idle
 * wait can be a real sleep instead of one tick. */
uint32_t esp32link_idle_ticks(void);
int      esp32link_hello_ok(void);
int      esp32link_link_is_up(void);

/* Drain whatever the ESP32 already pushed into the BRAM FIFO
 * (browser mouse/keyboard arrive unsolicited) and dispatch it.
 * Returns how many input events were applied this call. screen.c
 * calls this between stripes so a 300-700 ms scan does not hold
 * the pointer behind the framebuffer. */
int      esp32link_pump_input(void);
/* Button bits of the visor pointer, or 0 if present is down. Used
 * to postpone a full-frame force-resend while a drag is in flight. */
int      esp32link_vmouse_buttons(void);

#endif
