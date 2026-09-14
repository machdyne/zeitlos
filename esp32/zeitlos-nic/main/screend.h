#ifndef SCREEND_H
#define SCREEND_H
/* Serves the Zeitlos desktop to a browser: net streams framebuffer
 * stripes here over UDP (sw/apps/net/screen.c), an HTTP server on :80
 * serves a canvas page, and a WebSocket relays the stripes live. */
void screend_start(void);
#endif
