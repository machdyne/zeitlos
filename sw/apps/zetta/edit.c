/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * `zetta FILE`: the session. docs/zetta.md, "zetta FILE".
 *
 * A file's lines are zetta's paragraphs: shown wrapped, saved as they
 * were -- a line stays one line however long, as nano's soft wrap keeps
 * it. Saving writes a new file and renames it over the old, so a failed
 * save leaves the old one whole.
 */
#include <stdio.h>
#include <string.h>

#include "edit.h"
#include "../../common/zetta.h"
#include "../../common/zplat.h"

enum { A_SAVE = 1, A_SAVEAS = 2 };
enum { T_SAVEAS = 1, T_LEAVE = 2 };

static zetta_t g_ed;
static char g_text[ZE_TEXT_MAX];
static char g_clip[4096];
static char g_path[128], g_ask[128], g_title[160];
static bool g_done, g_leave_after_save;

static void set_title(void) {
	snprintf(g_title, sizeof(g_title), "zetta -- %s", g_path[0] ? g_path : "(no name yet)");
	g_ed.cfg.title = g_title;
	zetta_refresh(&g_ed);
}

// The text, into a new file renamed over `path`. False, having said why.
static bool save_to(const char *path) {
	char tmp[140];
	if (!path[0]) { zetta_error(&g_ed, "No name yet: Save as, from the menu (Esc)."); return false; }
	snprintf(tmp, sizeof(tmp), "%s.~", path);
	int h = plat_open(tmp, PLAT_CREATE);
	if (h < 0) { zetta_error(&g_ed, "Cannot save there: the file could not be made."); return false; }
	bool ok = g_ed.len == 0 || plat_write(h, g_ed.buf, (int)g_ed.len) == (int)g_ed.len;
	plat_close(h);
	if (!ok || !plat_rename(tmp, path)) {
		plat_unlink(tmp);
		zetta_error(&g_ed, "Could not save -- the file is as it was.");
		return false;
	}
	g_ed.modified = false;
	char msg[180];
	snprintf(msg, sizeof(msg), "Saved: %lu bytes to %s.", (unsigned long)g_ed.len, path);
	zetta_status(&g_ed, msg);
	return true;
}

static void ask_name(void) {
	snprintf(g_ask, sizeof(g_ask), "%s", g_path);
	zetta_prompt(&g_ed, "Save as:", g_ask, sizeof(g_ask), T_SAVEAS);
}

static void save(void) {
	if (!g_path[0]) { ask_name(); return; }
	if (save_to(g_path) && g_leave_after_save) g_done = true;
}

static bool event(int r) {
	switch (r) {
	case ZE_ACTION:
		if (g_ed.action == A_SAVE) save();
		else if (g_ed.action == A_SAVEAS) ask_name();
		break;
	case ZE_EXIT:
		if (!g_ed.modified) { g_done = true; break; }
		{
			char q[180];
			snprintf(q, sizeof(q), "Save changes to %s?", g_path[0] ? g_path : "a new file");
			zetta_confirm(&g_ed, q, T_LEAVE);
		}
		break;
	case ZE_ANSWER:
		if (g_ed.answer_tag == T_SAVEAS) {
			if (g_ed.answer == 'y' && g_ask[0]) {
				snprintf(g_path, sizeof(g_path), "%s", g_ask);
				set_title();
				save();
			} else g_leave_after_save = false;
		} else if (g_ed.answer_tag == T_LEAVE) {
			if (g_ed.answer == 'n') g_done = true;
			else if (g_ed.answer == 'y') { g_leave_after_save = true; save(); }
			// Esc: stay
		}
		break;
	}
	if (g_done) g_ed.cfg.write(g_ed.cfg.ctx, "\x1b[0m\x1b[2J\x1b[H", 11);	// a clear screen for the shell
	return g_done;
}

bool ze_open(const char *path, int rows, int cols,
		void (*write)(void *ctx, const char *b, uint32_t n), void *ctx, char *why, int cap) {
	snprintf(g_path, sizeof(g_path), "%s", path ? path : "");
	g_text[0] = 0;
	g_done = g_leave_after_save = false;
	bool is_new = true;
	if (g_path[0]) {
		int32_t size = plat_size(g_path);
		if (size >= 0) {
			is_new = false;
			if (size > ZE_TEXT_MAX - 4096) {
				snprintf(why, (size_t)cap, "%s is %ld bytes: zetta takes up to %d.", g_path, (long)size, ZE_TEXT_MAX - 4096);
				return false;
			}
			int h = plat_open(g_path, PLAT_READ);
			int n = h >= 0 ? plat_read(h, g_text, (int)size) : -1;
			if (h >= 0) plat_close(h);
			if (n != size) { snprintf(why, (size_t)cap, "%s could not be read.", g_path); return false; }
			// CRLF read as LF; a NUL means it is not text
			int o = 0;
			for (int i = 0; i < n; i++) {
				if (g_text[i] == 0) { snprintf(why, (size_t)cap, "%s is not text (a NUL byte at %d).", g_path, i); return false; }
				if (g_text[i] == '\r' && i + 1 < n && g_text[i + 1] == '\n') continue;
				g_text[o++] = g_text[i];
			}
			g_text[o] = 0;
		}
	}
	zetta_cfg_t c;
	memset(&c, 0, sizeof(c));
	c.rows = rows;
	c.cols = cols;
	c.actions[0].key = ZK_CTRL('O'); c.actions[0].label = "Save"; c.actions[0].id = A_SAVE;
	c.actions[1].key = 0; c.actions[1].label = "Save as..."; c.actions[1].id = A_SAVEAS;
	c.nactions = 2;
	c.main_action = A_SAVE;
	c.clip = g_clip;
	c.clip_cap = sizeof(g_clip);
	c.write = write;
	c.ctx = ctx;
	c.title = "zetta";
	zetta_init(&g_ed, &c, g_text, sizeof(g_text));
	set_title();
	if (is_new) zetta_status(&g_ed, g_path[0] ? "A new file." : "A new file: Ctrl-O asks for its name.");
	zetta_redraw(&g_ed);
	return true;
}

bool ze_input(const uint8_t *d, uint32_t n, uint32_t now_ms) {
	return event(zetta_key(&g_ed, d, n, now_ms));
}

bool ze_tick(uint32_t now_ms) {
	return event(zetta_tick(&g_ed, now_ms));
}
