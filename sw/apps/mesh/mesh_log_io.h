#ifndef MESH_LOG_IO_H
#define MESH_LOG_IO_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Message history on the card. See mesh_log.h.
 */

#include <stdint.h>
#include <stdbool.h>

#include "mesh_log.h"

// Read the history into the model; returns the messages loaded (0 for
// no file, which is not an error).
uint32_t mesh_log_load(mesh_model_t *m, const char *path);

// Append a message / a status change. false if it could not be written
// -- no card, a full one, a read-only one -- after which mesh stops
// trying and says so (mesh_log_io_ok()).
bool mesh_log_msg(mesh_model_t *m, const mesh_msg_t *g, const char *path);
bool mesh_log_status(const mesh_msg_t *g, const char *path);

bool mesh_log_io_ok(void);

#endif
