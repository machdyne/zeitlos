#ifndef MESH_LOG_H
#define MESH_LOG_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Message history, kept on the card in /user/mesh.log. See
 * docs/mesh_app.md, "History".
 *
 * -- the file --
 *
 * Plain text, one record per line, fields separated by tabs, so that it
 * can be read with `cat` and fixed with a text editor:
 *
 *   M <time> <from> <to> <chan> <id> <status> <err> <text>
 *   S <id> <status> <err>
 *
 * M is a message, received or sent; S is a later status change of one
 * we sent (sent, delivered, failed...), which on loading is applied to
 * the most recent M with that id. Numbers are decimal except node
 * numbers and packet ids, which are 8 hex digits as the Meshtastic apps
 * show them. In the text, a tab is \t, a newline \n and a backslash \\.
 * A line that does not parse is skipped, so a card pulled mid-write
 * costs that one record.
 *
 * The first line is "# mesh log v1"; a later version can tell its files
 * from this one's.
 *
 * This half is pure -- formatting and parsing, no files -- so the host
 * tests run it. mesh_log_io.c does the reading and writing.
 */

#include <stdint.h>

#include "mesh_model.h"

#define MESH_LOG_PATH		"/user/mesh.log"
#define MESH_LOG_HEADER		"# mesh log v1\n"
#define MESH_LOG_LINE_MAX	(MESH_PAYLOAD_MAX * 2 + 96)	// every byte escaped

// Past this size the log is rewritten as just the messages held in
// memory -- the newest MESH_MAX_MSGS -- so it cannot fill the card.
#define MESH_LOG_MAX_BYTES	(256u * 1024u)

// On start, only this much of the end of the file is read: far more
// than MESH_MAX_MSGS messages' worth, and bounded however large the
// file has grown.
#define MESH_LOG_TAIL_BYTES	(64u * 1024u)

// A record for g into out (cap bytes), with its newline. Returns the
// length, or 0 if it does not fit.
int mesh_log_fmt_msg(const mesh_msg_t *g, char *out, int cap);
int mesh_log_fmt_status(const mesh_msg_t *g, char *out, int cap);

// Feeds bytes of a log file, in any chunks; each complete line is
// applied to the model (M: a new message, S: a status change). Lines
// longer than MESH_LOG_LINE_MAX are skipped whole.
typedef struct {
	char line[MESH_LOG_LINE_MAX + 1];
	int n;
	int overlong;		// in a line that is being skipped
	uint32_t loaded;	// M records applied
	uint32_t bad;		// lines that did not parse
} mesh_log_rd_t;

void mesh_log_rd_init(mesh_log_rd_t *rd);
void mesh_log_feed(mesh_log_rd_t *rd, mesh_model_t *m, const char *p, int len);
// The last line, if the file did not end in a newline.
void mesh_log_finish(mesh_log_rd_t *rd, mesh_model_t *m);

// One line, without its newline. 1 if applied, 0 if skipped.
int mesh_log_apply(mesh_model_t *m, const char *line);

#endif
