#ifndef MDNS_H
#define MDNS_H

/*
 * Zeitlos
 *
 * DNS-SD browsing over multicast DNS (RFC 6763 over RFC 6762): "which
 * printers are on this network?" Exposed to other processes as
 * Z_NET_BROWSE / _REPLY (znet.h); sw/common/zbrowse.h wraps them.
 *
 * Queries only -- net announces nothing and answers nobody's queries.
 * And only "legacy unicast" queries (RFC 6762 section 6.7): sent to
 * 224.0.0.251:5353 from an ordinary port, which obliges every responder
 * to answer by unicast, straight back to that port. So nothing here
 * needs a MAC that takes multicast frames -- the ENC28J60's filter
 * stays unicast and broadcast only, and a busy network's mDNS chatter
 * never reaches the CPU. See docs/networking.md, "Finding services".
 *
 * One browse at a time, as one DNS lookup at a time (dns.c): a second
 * request while one runs gets an immediate "busy" reply.
 */

#include <stdint.h>

// Z_NET_BROWSE: `services` is one or more service types, separated by
// spaces ("_ipp._tcp _pdl-datastream._tcp"). Always answers exactly
// once, to (pid, tag), within about two seconds.
void mdns_browse_start(const char *services, uint32_t pid, uint32_t tag);

// every main-loop pass: the browse's schedule -- one test when idle
void mdns_poll(void);

#endif
