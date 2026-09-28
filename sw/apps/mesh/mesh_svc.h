#ifndef MESH_SVC_H
#define MESH_SVC_H
/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * mesh0: the radio as a service, for other programs -- zfed's radio link
 * first (docs/fed.md, "Constrained links"). docs/mesh_app.md, "mesh0".
 *
 * mesh stays the only program that speaks Meshtastic; a client sends and
 * receives payloads on an application's port (256 and up) through this,
 * one record in each port DATA:
 *
 *   client -> mesh   'L' port(2)                           listen on a port
 *                    'S' to(4) channel(1) port(2) payload  send (<= 233 bytes)
 *   mesh -> client   'U' my_num(4)                         the radio is up
 *                    'D'                                   ... and down
 *                    'R' from(4) to(4) channel(1) port(2) payload   one arrived
 *                    'E' text                              a send refused, and why
 *
 * Numbers little-endian. A client too far behind (eight records unacked)
 * misses packets rather than stalling mesh: a radio link must recover
 * from loss anyway.
 */
#include <stdbool.h>
#include "zeitlos.h"
#include "mesh_proto.h"
#include "mesh_session.h"

#define MESH_SVC_CLIENTS 4

// Registers "mesh" (mesh0). False if another mesh has it: then no service.
bool mesh_svc_start(void (*line)(const char *s));
// A message for the service? Handled, and true.
bool mesh_svc_msg(const z_msg_t *m, mesh_session_t *s);
// A session event: private-port packets go to their listeners.
void mesh_svc_event(const mesh_ev_t *ev);
// Once a pass: the radio's up or down told, gone clients forgotten.
void mesh_svc_tick(mesh_session_t *s);

#endif
