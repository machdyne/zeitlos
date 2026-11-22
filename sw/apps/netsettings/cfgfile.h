#ifndef CFGFILE_H
#define CFGFILE_H
/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * net.cfg as netsettings edits it: the file's lines, kept as they are
 * -- comments, blank lines, order, keys it does not know -- with single
 * keys read, changed, added and taken out. A key it changes stays on
 * its line; a new one goes at the end. Keys that may come more than
 * once (route, portfwd) are left to the file: netsettings does not
 * touch them.
 */

#define CF_LINES	256
#define CF_LINE		128

typedef struct {
	int n;
	char line[CF_LINES][CF_LINE];
} cf_t;

// Split text into lines. 0, or -1 when there are too many or one is
// too long (the file is then not edited).
int cf_load(cf_t *c, const char *text);

// The value of key (its last line), or 0.
const char *cf_get(const cf_t *c, const char *key);

// Set key: its first line keeps its place with the new value, any later
// ones go; with none, a line at the end. 0, or -1 when full.
int cf_set(cf_t *c, const char *key, const char *value);

// Take out every line of key; of every key starting with prefix.
void cf_unset(cf_t *c, const char *key);
void cf_unset_prefix(cf_t *c, const char *prefix);

// Is any key starting with prefix there?
int cf_has_prefix(const cf_t *c, const char *prefix);

// The file again, into out (cap bytes): its length, or -1.
int cf_save(const cf_t *c, char *out, int cap);

// "a.b.c.d" or "a.b.c.d/len": 1 if well formed.
int cf_valid_ip(const char *s, int allow_prefix);

#endif
