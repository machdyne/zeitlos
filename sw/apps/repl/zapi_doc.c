/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Looking things up in zapi.txt -- see zapi_doc.h.
 *
 * The file is read a chunk at a time through a 256-byte buffer, never
 * whole: it is about 30KB, and a lookup is a one-off that should not
 * need repl's heap to have that much free in one piece.
 */

#include <stdio.h>
#include <string.h>

#include "zapi_doc.h"

typedef struct {
	zdoc_read_fn	rd;
	void			*ctx;
	char			buf[256];
	int				len, pos;
	int				eof;
} zdoc_in_t;

static void zdoc_in_init(zdoc_in_t *in, zdoc_read_fn rd, void *ctx) {
	in->rd = rd;
	in->ctx = ctx;
	in->len = in->pos = 0;
	in->eof = 0;
}

// The next line, without its "\n" (or "\r\n"), cut at ZDOC_LINE - 1.
// 0 at the end of the file.
static int zdoc_getline(zdoc_in_t *in, char *line) {

	int n = 0;

	for (;;) {
		if (in->pos >= in->len) {
			if (in->eof) break;
			in->len = in->rd(in->ctx, in->buf, (int)sizeof(in->buf));
			in->pos = 0;
			if (in->len <= 0) {
				in->len = 0;
				in->eof = 1;
				break;
			}
		}
		char c = in->buf[in->pos++];
		if (c == '\n') {
			line[n] = 0;
			return 1;
		}
		if (c == '\r') continue;
		if (n < ZDOC_LINE - 1) line[n++] = c;
	}

	// a last line with no newline after it
	line[n] = 0;
	return n > 0;

}

// Does this call-shape line, "(name ...", name `name`?
static int zdoc_names(const char *line, const char *name) {
	size_t nl = strlen(name);
	if (line[0] != '(' || strncmp(line + 1, name, nl)) return 0;
	return line[1 + nl] == ' ' || line[1 + nl] == ')';
}

static char zdoc_lower(char c) {
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

// strstr(), ignoring ASCII case
static int zdoc_contains(const char *s, const char *text) {
	if (!*text) return 1;
	for (; *s; s++) {
		const char *a = s, *b = text;
		while (*a && *b && zdoc_lower(*a) == zdoc_lower(*b)) { a++; b++; }
		if (!*b) return 1;
	}
	return 0;
}

int zdoc_help(const char *name, zdoc_read_fn rd, void *ctx) {

	zdoc_in_t in;
	char line[ZDOC_LINE];
	// An entry can have several names -- (caar x), (cadr x), ... -- so
	// its call shapes are kept until the first description line says
	// the entry's head is over, then printed if any of them matched.
	char shapes[512];
	int shapes_len = 0, in_shapes = 0, found = 0, printed = 0;
	int prev_blank = 1;

	zdoc_in_init(&in, rd, ctx);

	while (zdoc_getline(&in, line)) {

		if (printed) {
			if (!line[0]) break;
			printf("%s\n", line);
			printed++;
			continue;
		}

		if (line[0] == '(' && (prev_blank || in_shapes)) {
			if (!in_shapes) shapes_len = found = 0;
			in_shapes = 1;
			if (zdoc_names(line, name)) found = 1;
			int n = (int)strlen(line);
			if (shapes_len + n + 1 < (int)sizeof(shapes)) {
				memcpy(shapes + shapes_len, line, (size_t)n);
				shapes_len += n;
				shapes[shapes_len++] = '\n';
			}
		} else if (in_shapes) {
			in_shapes = 0;
			if (found) {
				fwrite(shapes, 1, (size_t)shapes_len, stdout);
				printed = 0;
				for (int i = 0; i < shapes_len; i++)
					if (shapes[i] == '\n') printed++;
				if (!line[0]) break;
				printf("%s\n", line);
				printed++;
			}
		}

		prev_blank = !line[0];

	}

	return printed;

}

int zdoc_apropos(const char *text, zdoc_read_fn rd, void *ctx) {

	zdoc_in_t in;
	char line[ZDOC_LINE];
	char name[24];
	char desc[64];
	int in_entry = 0, matched = 0, count = 0, prev_blank = 1, done = 0;

	zdoc_in_init(&in, rd, ctx);
	name[0] = desc[0] = 0;

	while (!done) {

		if (!zdoc_getline(&in, line)) {
			done = 1;
			line[0] = 0;
		}

		if (!line[0]) {
			if (in_entry && matched) {
				if (count < ZDOC_APROPOS_MAX)
					printf("%-16s %s\n", name, desc);
				count++;
			}
			in_entry = 0;
			prev_blank = 1;
			continue;
		}

		if (prev_blank && line[0] == '(') {
			// the name: up to the first space or ')'
			int n = 0;
			const char *p = line + 1;
			while (*p && *p != ' ' && *p != ')' && n < (int)sizeof(name) - 1)
				name[n++] = *p++;
			name[n] = 0;
			desc[0] = 0;
			in_entry = 1;
			matched = 0;
		}
		prev_blank = 0;

		if (!in_entry) continue;

		int example = line[0] == ' ' && line[1] == ' ' && line[2] == '>';
		if (example) continue;

		// the first description line, as much as fits beside the name
		if (!desc[0] && line[0] == ' ') {
			const char *p = line;
			while (*p == ' ') p++;
			strncpy(desc, p, sizeof(desc) - 1);
			desc[sizeof(desc) - 1] = 0;
			if (strlen(p) > 60) {
				// cut at a word, so 16 + 1 + 60 fits an 80-column line
				int cut = 57;
				while (cut > 20 && desc[cut] != ' ') cut--;
				if (desc[cut] != ' ') cut = 57;		// one long word
				while (cut > 0 && strchr(".,;:", desc[cut - 1])) cut--;
				strcpy(desc + cut, "...");
			}
		}

		if (zdoc_contains(line, text)) matched = 1;

	}

	if (count > ZDOC_APROPOS_MAX)
		printf("... and %d more: (apropos \"something narrower\")\n",
			count - ZDOC_APROPOS_MAX);

	return count;

}
