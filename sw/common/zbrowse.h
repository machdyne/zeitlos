#ifndef ZBROWSE_H
#define ZBROWSE_H

/*
 * Zeitlos
 *
 * Finding services on the network -- printers, for now -- by DNS-SD
 * over multicast DNS, asked of `net` (Z_NET_BROWSE, znet.h;
 * sw/apps/net/mdns.c). docs/networking.md, "Finding services".
 *
 * A search takes about two seconds, so there are two ways to use it:
 *
 *   z_browse_printers()          blocks until the answer: for a command
 *                                ("print find") that has nothing else to do
 *
 *   z_browse_ask()  then         for a window that must go on drawing:
 *   z_browse_reply()             ask, keep reading messages, offer each
 *                                one to z_browse_reply() (the print dialog)
 */

#include <stdint.h>
#include <stdbool.h>

#include "zmsg.h"		// z_msg_t

#define Z_BROWSE_MAX        8
#define Z_BROWSE_NAME_MAX   48
#define Z_BROWSE_URI_MAX    64

typedef struct {
	char name[Z_BROWSE_NAME_MAX];	// the model (TXT ty=), or the service's own name
	char uri[Z_BROWSE_URI_MAX];		// "ipp://10.0.0.5:631/ipp/print", "socket://10.0.0.7:9100"
} z_browse_printer_t;

// The service types a printer is found by: IPP, and raw PostScript on
// port 9100 ("_pdl-datastream"). Not "_ipps" (no IPP over TLS in print)
// nor "_printer" (LPD, which print does not speak).
#define Z_BROWSE_PRINTERS   "_ipp._tcp _pdl-datastream._tcp"

// Asks net. The tag to look for, or 0 with a reason in err (net is not
// running).
uint32_t z_browse_ask(const char *services, char *err, uint32_t errlen);

// Is msg the answer to `tag`? -1 not this one; else the number of
// printers put in out[] (up to max) -- 0 also when the search failed,
// with the reason in err. A printer offered over both IPP and port 9100
// is listed once, by IPP.
int z_browse_reply(const z_msg_t *msg, uint32_t tag, z_browse_printer_t *out, int max,
	char *err, uint32_t errlen);

// Both, waiting up to five seconds; other messages stay queued.
int z_browse_printers(z_browse_printer_t *out, int max, char *err, uint32_t errlen);

// For tests and for z_browse_reply(): one "found" text (znet.h) into
// printers.
int z_browse_parse(const char *found, z_browse_printer_t *out, int max);

#endif
