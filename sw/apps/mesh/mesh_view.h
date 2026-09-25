#ifndef MESH_VIEW_H
#define MESH_VIEW_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * What the mesh window shows, decided without drawing anything: the
 * conversation list and its unread counts, which messages belong to a
 * conversation, word wrap, and the input line. No Zeitlos dependency;
 * tests/test_view.c runs it on the host. mesh_ui.c draws it.
 */

#include <stdint.h>
#include <stdbool.h>

#include "mesh_model.h"

// -- conversations --

#define CONV_CHAN	0		// a channel: everyone on it
#define CONV_DM		1		// one node

#define MESH_MAX_CONV	(MESH_MAX_CHANNELS + MESH_MAX_NODES)
#define MESH_DM_TRACK	48	// nodes whose unread count / activity we keep

typedef struct {
	uint8_t kind;
	uint8_t chan;			// CONV_CHAN
	uint32_t peer;			// CONV_DM
	uint16_t unread;
	uint32_t last;			// seq of its newest message, 0 none
	uint32_t heard;			// CONV_DM: the node's last_heard, for sorting
} mesh_conv_t;

typedef struct {
	// The list as last built. Channels first, in index order; then
	// nodes -- those with messages, newest first, then the rest, most
	// recently heard first. Our own node is not in it.
	mesh_conv_t conv[MESH_MAX_CONV];
	int nconv;

	// Unread counts and activity, kept apart from the list so a rebuild
	// (a new node, a reconnect that refilled the node DB) keeps them.
	uint16_t chan_unread[MESH_MAX_CHANNELS];
	uint32_t chan_last[MESH_MAX_CHANNELS];
	struct { uint32_t num; uint16_t unread; uint32_t last; } dm[MESH_DM_TRACK];

	// The conversation shown. Identified by what it is, not where it
	// is in the list, because the list reorders under it.
	uint8_t cur_kind;
	uint8_t cur_chan;
	uint32_t cur_peer;
} mesh_view_t;

void mesh_view_init(mesh_view_t *v);

// The conversation a message belongs to, given which node we are.
void mesh_view_conv_of(const mesh_model_t *m, const mesh_msg_t *g,
	uint8_t *kind, uint8_t *chan, uint32_t *peer);

// A message arrived, or we sent one: note activity, and count it
// unread unless it is ours or its conversation is the one shown.
void mesh_view_note(mesh_view_t *v, const mesh_model_t *m, const mesh_msg_t *g);

void mesh_view_build(mesh_view_t *v, mesh_model_t *m);

// The shown conversation's index in the list; if it is not in the list
// (its node was evicted, its channel disabled) the first entry is
// selected instead. -1 only if the list is empty.
int mesh_view_cur(mesh_view_t *v);

// Show list entry i and mark it read.
void mesh_view_select(mesh_view_t *v, int i);

bool mesh_view_in_cur(const mesh_view_t *v, const mesh_model_t *m,
	const mesh_msg_t *g);

uint32_t mesh_view_unread_total(const mesh_view_t *v);

// Everything read: after loading the history.
void mesh_view_mark_read(mesh_view_t *v);

// -- word wrap --

// Lines of at most `cols` columns (CJK counts two) from UTF-8 `s`,
// breaking after a space where one is in reach, otherwise mid-word;
// '\n' always breaks. Lines after the first are `indent` columns
// shorter (the caller indents them). Calls emit() for each line with
// its byte range; returns the number of lines. emit may be NULL to
// count only.
int mesh_wrap(const char *s, int cols, int indent,
	void (*emit)(void *ctx, const char *p, int len, int line), void *ctx);

// -- the input line --

typedef struct {
	char buf[MESH_PAYLOAD_MAX + 1];
	int len;				// bytes
	int cur;				// byte offset of the cursor
} mesh_input_t;

void mesh_input_clear(mesh_input_t *in);
// Insert a codepoint at the cursor. false if it would not fit in a
// message (MESH_PAYLOAD_MAX bytes) or is not a character.
bool mesh_input_insert(mesh_input_t *in, uint32_t cp);
void mesh_input_backspace(mesh_input_t *in);
void mesh_input_delete(mesh_input_t *in);
void mesh_input_left(mesh_input_t *in);
void mesh_input_right(mesh_input_t *in);
void mesh_input_home(mesh_input_t *in);
void mesh_input_end(mesh_input_t *in);
// Delete the word before the cursor (Ctrl+W).
void mesh_input_word(mesh_input_t *in);
// The byte offset to start drawing from so that the cursor is visible
// in `cols` columns; `start` is the previous one, kept when it works so
// the line does not jump on every key.
int mesh_input_scroll(const mesh_input_t *in, int start, int cols);

#endif
