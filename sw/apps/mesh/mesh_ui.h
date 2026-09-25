#ifndef MESH_UI_H
#define MESH_UI_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The mesh window: drawing and input. What to show is decided in
 * mesh_view.c; this file only lays it out and draws it. See
 * docs/mesh_app.md, "User interface".
 */

#include <stdint.h>
#include <stdbool.h>

#include "zeitlos.h"
#include "zwin.h"

#include "mesh_model.h"
#include "mesh_proto.h"
#include "mesh_session.h"

// What the transport is doing, for the status bar.
#define LINK_NO_SERIAL	0	// `serial` is not running
#define LINK_NO_DEVICE	1	// serial refused: no USB serial device, or busy
#define LINK_UP			2	// connected; the session says the rest

// Open the window. false if wm refused.
bool mesh_ui_open(mesh_model_t *m, mesh_session_t *s);

// A message for the window (a Z_WM_* subject). Returns false when the
// user has asked to quit.
bool mesh_ui_wm(z_msg_t *msg);

// From the session.
void mesh_ui_event(const mesh_ev_t *ev);
void mesh_ui_line(const char *s);
void mesh_ui_link(int link);

// Draw whatever changed since the last call.
void mesh_ui_flush(void);

void mesh_ui_close(void);

// A line for the status bar, shown for a few seconds.
void mesh_ui_notice(const char *s);

// Implemented by the caller (mesh.c): the window just sent a message.
// It is in the model; this is for the history on the card.
void mesh_app_sent(mesh_msg_t *g);

#endif
