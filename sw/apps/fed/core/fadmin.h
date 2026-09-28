#ifndef FADMIN_H
#define FADMIN_H
/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * A network's node list, managed from the command line -- on Linux
 * (`sudo fed nodes`) and on Zeitlos (`run fed nodes`) alike. docs/fed.md,
 * "Managing a network". fadmin.c; the platform brings what differs.
 *
 *   key                                      this node's PUBLIC key, and where its private one is
 *   nodes                                    each network's list
 *   add NAME PUBLIC-KEY [sysop=S] [addr=H:P] (its publisher) add a node; published at once
 *   set NAME [sysop=S] [addr=H:P] [name=N]   (its publisher) change one
 *   remove NAME                              (its publisher) remove one (NAME, key or short id)
 */
#include <stdint.h>
#include <stdbool.h>

typedef struct {
	// One request to the running fed's local interface: its first reply
	// line into `line`; after "OK n", n bytes into `data` (if not NULL).
	// The data's length, or -1 if fed could not be reached.
	int (*request)(const char *req, const uint8_t *payload, int plen,
		char *line, int lcap, uint8_t *data, int dcap, void *ctx);
	// This node's PUBLIC key -- never the private one. False: none yet.
	bool (*public_key)(uint8_t pk[32], void *ctx);
	// fed.cfg's text, NUL-terminated.
	bool (*read_cfg)(char *text, int cap, void *ctx);
	// A line to show the user (no newline in it).
	void (*out)(const char *line, void *ctx);
	const char *private_key_where;			// told with `key`: where the secret lives
	const char *how_to_start;				// told when fed is not running
	void *ctx;
} fadmin_io_t;

bool fadmin_is_command(const char *word);
// argv[0] is the command. 0 done; 1 refused (said why); 2 used wrongly.
int fadmin(const fadmin_io_t *io, const char *network, int argc, char **argv);

#endif
