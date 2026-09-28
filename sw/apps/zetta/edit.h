#ifndef ZETTA_EDIT_H
#define ZETTA_EDIT_H
/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * `zetta FILE` -- a file in the zetta editor (docs/zetta.md): opening,
 * saving, and nano's question on leaving. On zplat, so it runs (and is
 * tested) on the host as well; zetta_zeitlos.c hands it a terminal.
 */
#include <stdint.h>
#include <stdbool.h>

#define ZE_TEXT_MAX   (48 * 1024)		// static: the default memory tier has no room for it on the heap

// Opens `path` ("" for none yet): false, with why, if it cannot be edited.
bool ze_open(const char *path, int rows, int cols,
	void (*write)(void *ctx, const char *b, uint32_t n), void *ctx, char *why, int cap);
// Bytes typed; the time. True once the editor is finished.
bool ze_input(const uint8_t *d, uint32_t n, uint32_t now_ms);
bool ze_tick(uint32_t now_ms);

#endif
