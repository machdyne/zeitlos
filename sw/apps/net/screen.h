#ifndef NET_SCREEN_H
#define NET_SCREEN_H

#include <stdint.h>

#ifdef Z_SMALL
/* The small build (Obst) leaves screen streaming out: it runs over the
   ESP32 link, which Obst does not have, and its stripes' shadow is 38K of
   a 1MB machine's RAM (docs/flash_apps.md, "Two sizes of machine"). */
static inline void screen_poll(uint32_t gw_ip) { (void)gw_ip; }
static inline void screen_reset(void) { }
static inline void screen_set_viewer(int present) { (void)present; }
static inline uint32_t screen_idle_ticks(void) { return 0; }
#else

/* Pushes the 1bpp framebuffer to the gateway as UDP stripe packets so
 * it can serve the desktop to a browser -- see docs/esp32link.md and
 * the firmware's screend.c. Free when no browser is connected and when
 * a scan is not yet due: both are one comparison.
 *
 * Every stripe a scan ships belongs to ONE instant of the framebuffer
 * and says so on the wire; see screen.c on the snapshot and the frame
 * trailer. */
void screen_poll(uint32_t gw_ip);

/* Forget what was sent so the next poll ships the whole frame again --
 * call when the consumer's shadow may be empty (the ESP32 rebooted, or
 * a viewer has just connected). */
void screen_reset(void);

/* Whether a browser is watching. Nothing is hashed or sent while this
 * is 0. esp32link.c owns the answer -- the WebSocket terminates on the
 * ESP32, so net can only be told. */
void screen_set_viewer(int present);

/* Ticks until the next scan is due, for net's idle wait; 0 when there
 * is no viewer and therefore no deadline to keep. */
uint32_t screen_idle_ticks(void);

#endif	/* Z_SMALL */

#endif
