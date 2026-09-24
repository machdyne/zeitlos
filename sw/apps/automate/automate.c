/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * automate -- runs a demo script: speech, captions, keys, the pointer
 * and apps, in order. See docs/automate.md for the language and
 * docs/demo.md for the demos themselves.
 *
 *     > run automate                      runs /demo/demo.zds
 *     open a .zds file in `files`         runs that one
 *
 * Everything it does to the machine goes through the paths a person
 * uses: keys through hid_inject() (the on-screen keyboard's path), the
 * pointer through the virtual mouse register (the remote desktop's),
 * apps through z_proc_run(). What it needs from wm that a person gets
 * by looking -- where a window is -- comes from Z_WM_WIN_QUERY.
 *
 * A second copy stops the first: running it again is how you restart
 * a demo.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"
#include "../../common/zwm.h"
#include "../../common/zwin.h"
#include "../../common/zkbd.h"
#include "../../common/ztts.h"
#include "../../common/zcaption.h"
#include "../../common/zfsapp.h"
#include "../../common/zutf8.h"

#define DEFAULT_SCRIPT	"/demo/demo.zds"
#define SCRIPT_MAX		32768
#define LINES_MAX		1500
#define APPS_MAX		16
#define SAY_RING		8
#define SAY_MAX			600
#define MARKS_MAX		8

static uint32_t ms_ticks(uint32_t ms) { return ms * Z_TICK_HZ / 1000u + 1; }
static uint32_t now(void) { return z_uptime_ticks(); }

// -- the script --

static char script_buf[SCRIPT_MAX + 1];
static char *lines[LINES_MAX];
static int nlines;
static int pc;
static char script_path[Z_WM_ARG_MAX];

// -- state --

static bool aborted;			// a person touched the machine, or `stop`
static bool finished;			// `end`: done, or start over in attract mode
static bool stopped;			// `stop`: done, whatever the mode
static int attract_s;			// 0 = run once; else loop, idle this long after a touch
static bool interruptible = true;
static uint32_t real_base;		// wm's real-input count we started from
static uint32_t last_abort_check;

static struct { char name[16]; uint32_t pid; } apps[APPS_MAX];
static int origin_x, origin_y;

// -- messages --

static uint32_t tts_pid;
static char say_ring[SAY_RING][SAY_MAX];
static int say_slot;
static uint16_t next_mark = 1;
static struct { uint16_t mark; uint32_t deadline; } marks[MARKS_MAX];
static int nmarks;
static uint32_t speech_until;	// estimated, when there is no voice
static uint32_t lease_sent;
static int rate_wpm = 180;

static z_wm_win_info_t info;	// last Z_WM_WIN_INFO
static bool info_ready;
static uint32_t info_tag;

static void mark_done(uint16_t mk) {
	for (int i = 0; i < nmarks; i++)
		if (marks[i].mark == mk) { marks[i] = marks[--nmarks]; return; }
}

static void handle_msg(z_msg_t *m) {
	switch (m->subject) {
	case Z_TTS_MARK_DONE:
	case Z_TTS_MARK_CANCELLED:
		mark_done((uint16_t)(m->tag & 0xffff));
		break;
	case Z_WM_WIN_INFO:
		if (m->tag == info_tag && z_blob_len(&m->obj) >= sizeof(info)) {
			memcpy(&info, z_blob_data(&m->obj), sizeof(info));
			info_ready = true;
		}
		break;
	default:
		break;
	}
}

static void pump(void) {
	z_msg_t m;
	while (z_msg_read(&m) == Z_OK) handle_msg(&m);
}

static uint32_t wm_pid(void) {
	static uint32_t pid;
	if (!pid && !z_pid_lookup("wm0", &pid)) pid = Z_PID_WM;
	return pid;
}

// Asks wm about the frontmost window of `pid` (0: the focused one).
// Returns true if the answer came; info.found says if there was one.
static bool query(uint32_t pid) {
	info_ready = false;
	info_tag = pid;
	if (z_msg_new_send(wm_pid(), Z_WM_WIN_QUERY, pid, z_obj_none()) != Z_OK)
		return false;
	uint32_t end = now() + ms_ticks(500);
	while (!info_ready && (int32_t)(now() - end) < 0) {
		z_proc_wait(ms_ticks(20));
		pump();
	}
	return info_ready;
}

// -- abort: a person at the machine --

static void check_abort(void) {
	if (aborted || !interruptible) return;
	if ((int32_t)(now() - last_abort_check) < (int32_t)ms_ticks(150)) return;
	last_abort_check = now();
	if (query(0) && info.real_input != real_base) {
		printf("automate: real input (%lu events; wm's log says what) -- stopping\n",
			(unsigned long)(info.real_input - real_base));
		aborted = true;
	}
}

// The one wait everything uses: services messages, notices a person.
static bool wait_ms(uint32_t ms) {
	uint32_t end = now() + ms_ticks(ms);
	for (;;) {
		pump();
		check_abort();
		if (aborted) return false;
		int32_t left = (int32_t)(end - now());
		if (left <= 0) return true;
		z_proc_wait((uint32_t)(left > 40 ? 40 : left));
	}
}

// -- speech --

static void narrator_lease(void) {
	if (!tts_pid) return;
	z_msg_new_send(tts_pid, Z_TTS_NARRATE, 0, z_obj_uint32(30));
	lease_sent = now();
}

static bool ensure_tts(void) {
	if (tts_pid && z_pid_lookup(Z_TTS_SERVICE, &tts_pid)) return true;
	tts_pid = 0;
	if (z_pid_lookup(Z_TTS_SERVICE, &tts_pid)) { narrator_lease(); return true; }
	if (!z_exec_exists("tts")) return false;
	printf("automate: starting tts\n");
	z_proc_run("tts");
	uint32_t end = now() + ms_ticks(5000);
	while ((int32_t)(now() - end) < 0) {
		z_proc_wait(ms_ticks(50));
		pump();
		if (z_pid_lookup(Z_TTS_SERVICE, &tts_pid)) {
			narrator_lease();
			// "Speech on" is not part of the demo. A STOP from the
			// narrator is allowed.
			z_msg_new_send(tts_pid, Z_TTS_STOP, 0, z_obj_none());
			return true;
		}
	}
	tts_pid = 0;
	return false;
}

static uint32_t speech_estimate_ms(const char *t) {
	uint32_t len = (uint32_t)strlen(t);
	return len * 60000u / ((uint32_t)rate_wpm * 6u) + 300;
}

static void say(const char *text, bool wait);

static bool sync_speech(void) {
	for (;;) {
		uint32_t t = now();
		for (int i = 0; i < nmarks; ) {
			if ((int32_t)(t - marks[i].deadline) >= 0) {
				printf("automate: speech mark %u timed out\n", marks[i].mark);
				marks[i] = marks[--nmarks];
			} else i++;
		}
		if (!nmarks && (int32_t)(t - speech_until) >= 0) return true;
		if ((int32_t)(t - lease_sent) > (int32_t)ms_ticks(10000)) narrator_lease();
		if (!wait_ms(20)) return false;
	}
}

static void say(const char *text, bool wait) {
	if (!*text) return;
	uint32_t est = speech_estimate_ms(text);
	if (!ensure_tts() || nmarks >= MARKS_MAX) {
		// No voice: keep the timing a voice would have had, so the
		// captions still stay up long enough to read.
		uint32_t base = (int32_t)(speech_until - now()) > 0 ? speech_until : now();
		speech_until = base + ms_ticks(est);
	} else {
		char *buf = say_ring[say_slot];
		say_slot = (say_slot + 1) % SAY_RING;
		strncpy(buf, text, SAY_MAX - 1);
		buf[SAY_MAX - 1] = 0;
		uint16_t mk = next_mark++;
		if (!next_mark) next_mark = 1;
		z_obj_t o;
		o.type = Z_STR;
		o.val.str = buf;
		if (z_msg_new_send(tts_pid, Z_TTS_SAY, Z_TTS_TAG(0, mk), o) == Z_OK) {
			marks[nmarks].mark = mk;
			// Generous: the recorded voice reads the card.
			marks[nmarks].deadline = now() + ms_ticks(est * 2 + 5000);
			nmarks++;
			lease_sent = now();
		}
	}
	if (wait) sync_speech();
}

// -- captions --

static int cap_scale = 2;
static uint32_t cap_pos = Z_CAPTION_BOTTOM;
static uint32_t cap_flags;

static void caption(const char *text) {
	if (!*text) { z_caption_hide(); return; }
	z_caption_show(text, Z_CAPTION_SCALE(cap_scale) | cap_pos | cap_flags);
}

// -- keys --

static const struct { const char *name; uint8_t usage; } named_keys[] = {
	{ "enter", 0x28 }, { "return", 0x28 }, { "esc", 0x29 }, { "escape", 0x29 },
	{ "backspace", 0x2a }, { "bs", 0x2a }, { "tab", 0x2b }, { "space", 0x2c },
	{ "minus", 0x2d }, { "equal", 0x2e }, { "caps", 0x39 },
	{ "f1", 0x3a }, { "f2", 0x3b }, { "f3", 0x3c }, { "f4", 0x3d }, { "f5", 0x3e },
	{ "f6", 0x3f }, { "f7", 0x40 }, { "f8", 0x41 }, { "f9", 0x42 }, { "f10", 0x43 },
	{ "f11", 0x44 }, { "f12", 0x45 }, { "insert", 0x49 }, { "home", 0x4a },
	{ "pgup", 0x4b }, { "delete", 0x4c }, { "del", 0x4c }, { "end", 0x4d },
	{ "pgdn", 0x4e }, { "right", 0x4f }, { "left", 0x50 }, { "down", 0x51 },
	{ "up", 0x52 },
};

// What each character is on the ACTIVE layout: built from the same
// tables wm translates with (z_kbd_translate()), so it cannot disagree.
typedef struct { uint32_t cp; uint8_t usage, mods; } km_t;
static km_t km[400];
static int nkm;
static km_t dk[24];			// dead keys: cp holds the dead keysym
static int ndk;
static int km_layout = -1;

static void keymap_build(void) {
	static const uint8_t try_mods[4] = {
		0, Z_KBD_MOD_LSHIFT, Z_KBD_MOD_RALT, Z_KBD_MOD_RALT | Z_KBD_MOD_LSHIFT };
	int layout = z_kbd_active_layout();
	nkm = ndk = 0;
	for (int m = 0; m < 4; m++) {
		for (int u = 0x04; u <= 0x89; u++) {
			if (u > 0x38 && u != 0x64 && u != 0x87 && u != 0x89) continue;
			if (u == 0x28 || u == 0x29 || u == 0x2a || u == 0x2b) continue;
			uint32_t ks = z_kbd_translate(layout, (uint8_t)u, try_mods[m], 0, NULL);
			if (ks == Z_KEY_NONE) continue;
			if (Z_KEY_IS_DEAD(ks)) {
				int have = 0;
				for (int i = 0; i < ndk; i++) if (dk[i].cp == ks) have = 1;
				if (!have && ndk < (int)(sizeof(dk) / sizeof(dk[0]))) {
					dk[ndk].cp = ks; dk[ndk].usage = (uint8_t)u; dk[ndk].mods = try_mods[m]; ndk++;
				}
				continue;
			}
			if (!Z_KEY_IS_TEXT(ks)) continue;
			int have = 0;
			for (int i = 0; i < nkm; i++) if (km[i].cp == ks) { have = 1; break; }
			if (have || nkm >= (int)(sizeof(km) / sizeof(km[0]))) continue;
			km[nkm].cp = ks; km[nkm].usage = (uint8_t)u; km[nkm].mods = try_mods[m]; nkm++;
		}
	}
	km_layout = layout;
}

static const km_t *key_for(uint32_t cp) {
	if (km_layout != z_kbd_active_layout()) keymap_build();
	for (int i = 0; i < nkm; i++) if (km[i].cp == cp) return &km[i];
	return NULL;
}

static void inject(uint8_t usage, uint8_t mods, bool down) {
	hid_inject((int32_t)(((uint32_t)mods << 9) | ((uint32_t)usage << 1) | (down ? 1u : 0u)));
}

static bool tap(uint8_t usage, uint8_t mods, uint32_t hold_ms) {
	inject(usage, mods, true);
	if (!wait_ms(hold_ms ? hold_ms : 25)) { inject(usage, mods, false); return false; }
	inject(usage, mods, false);
	return wait_ms(15);
}

// "ctrl+shift+z", "alt+tab", "enter", "q", "0x2c". False if unknown.
static bool parse_combo(const char *s, uint8_t *usage, uint8_t *mods) {
	char tok[24];
	*mods = 0;
	*usage = 0;
	for (;;) {
		const char *plus = strchr(s, '+');
		int n = plus && plus[1] ? (int)(plus - s) : (int)strlen(s);
		if (n >= (int)sizeof(tok)) return false;
		memcpy(tok, s, n); tok[n] = 0;
		for (char *c = tok; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
		bool last = !(plus && plus[1]);
		if (!last) {
			if (!strcmp(tok, "ctrl")) *mods |= Z_KBD_MOD_LCTRL;
			else if (!strcmp(tok, "shift")) *mods |= Z_KBD_MOD_LSHIFT;
			else if (!strcmp(tok, "alt")) *mods |= Z_KBD_MOD_LALT;
			else if (!strcmp(tok, "altgr")) *mods |= Z_KBD_MOD_RALT;
			else if (!strcmp(tok, "super") || !strcmp(tok, "win")) *mods |= Z_KBD_MOD_LGUI;
			else return false;
			s = plus + 1;
			continue;
		}
		for (unsigned i = 0; i < sizeof(named_keys) / sizeof(named_keys[0]); i++)
			if (!strcmp(tok, named_keys[i].name)) { *usage = named_keys[i].usage; return true; }
		if (tok[0] == '0' && tok[1] == 'x') { *usage = (uint8_t)strtol(tok, NULL, 16); return *usage != 0; }
		const char *p = s;
		uint32_t cp = z_utf8_next_z(&p);
		if (cp >= 'A' && cp <= 'Z') cp += 32;
		const km_t *k = key_for(cp);
		if (!k) return false;
		*usage = k->usage;
		*mods |= k->mods;
		return true;
	}
}

static uint32_t rng = 12345;
static uint32_t jitter(uint32_t ms) {
	rng = rng * 1103515245u + 12345u;
	return ms * (70 + ((rng >> 16) % 61)) / 100;	// 70%..130%
}

static uint32_t type_ms = 55;

static bool type_char(uint32_t cp) {
	uint8_t u = 0, m = 0;
	if (cp == '\n') u = 0x28;
	else if (cp == '\t') u = 0x2b;
	if (u) return tap(u, 0, 20);
	const km_t *k = key_for(cp);
	if (k) return tap(k->usage, k->mods, 20);
	// Not on a key of its own: a dead key and then a base letter.
	for (int d = 0; d < ndk; d++)
		for (int b = 0; b < nkm; b++)
			if (z_kbd_compose(dk[d].cp, km[b].cp) == cp) {
				if (!tap(dk[d].usage, dk[d].mods, 20)) return false;
				return tap(km[b].usage, km[b].mods, 20);
			}
	printf("automate: cannot type U+%04lx on this layout\n", (unsigned long)cp);
	(void)m;
	return true;
}

static bool type_text(const char *s) {
	while (*s) {
		uint32_t cp = z_utf8_next_z(&s);
		if (!type_char(cp)) return false;
		uint32_t d = jitter(type_ms);
		if (cp == ' ' || cp == ',' || cp == '.') d += type_ms / 2;
		if (!wait_ms(d)) return false;
	}
	return true;
}

// -- the pointer (the virtual mouse, Z_FEATURE2_VMOUSE) --

static bool have_ptr;
static int px = 320, py = 240, pbtn;

static void ptr_write(void) {
	if (!have_ptr) return;
	if (px < 0) px = 0;
	if (px > 639) px = 639;
	if (py < 0) py = 0;
	if (py > 479) py = 479;
	reg_vmouse = (1u << 24) | ((uint32_t)(pbtn & 7) << 20) |
		((uint32_t)py << 10) | (uint32_t)px;
	z_wm_wake();
}

static void ptr_release(void) {
	if (!have_ptr) return;
	reg_vmouse = 0;
	z_wm_wake();
}

static void ptr_init(void) {
	have_ptr = z_soc_has_feature2(Z_FEATURE2_VMOUSE);
	if (!have_ptr) {
		printf("automate: no virtual mouse in this bitstream -- keyboard only\n");
		return;
	}
	uint32_t c = reg_vmouse;
	if (!(c & (1u << 24))) {
		uint8_t typ1 = (reg_usb1_info >> 24) & 3;
		c = (typ1 == 2) ? reg_usb1_cursor : reg_usb0_cursor;
	}
	px = (int)(c & 0x3ff);
	py = (int)((c >> 10) & 0x3ff);
}

// Smoothstep glide, ~60 steps a second.
static bool glide(int x, int y, uint32_t ms) {
	if (!have_ptr) { px = x; py = y; return true; }
	int x0 = px, y0 = py;
	int steps = (int)(ms / 16);
	if (steps < 1) steps = 1;
	for (int i = 1; i <= steps; i++) {
		int32_t t = i * 1024 / steps;
		int32_t e = (3 * t * t - 2 * t * t / 1024 * t) / 1024;	// 0..1024
		px = x0 + (x - x0) * e / 1024;
		py = y0 + (y - y0) * e / 1024;
		ptr_write();
		if (!wait_ms(16)) return false;
	}
	px = x; py = y;
	ptr_write();
	return true;
}

static bool button(bool down) {
	pbtn = down ? 1 : 0;
	ptr_write();
	return wait_ms(down ? 50 : 40);
}

static bool click(void) {
	return button(true) && button(false);
}

// -- apps --

static int app_find(const char *name) {
	for (int i = 0; i < APPS_MAX; i++)
		if (apps[i].pid && !strcmp(apps[i].name, name)) return i;
	return -1;
}

static bool pid_alive(uint32_t pid) {
	static z_proc_info_t pl[32];
	uint32_t trunc;
	uint32_t n = z_proc_list(pl, 32, &trunc);
	for (uint32_t i = 0; i < n; i++) if (pl[i].pid == pid) return true;
	return false;
}

static uint32_t app_pid(const char *name) {
	int i = app_find(name);
	if (i >= 0 && pid_alive(apps[i].pid)) return apps[i].pid;
	if (i >= 0) apps[i].pid = 0;
	printf("automate: '%s' is not running\n", name);
	return 0;
}

static bool await_app(const char *name, uint32_t ms) {
	uint32_t pid = app_pid(name);
	if (!pid) return true;
	uint32_t end = now() + ms_ticks(ms);
	while ((int32_t)(now() - end) < 0) {
		if (query(pid) && info.found) {
			// Its first paint.
			return wait_ms(120);
		}
		if (!wait_ms(40)) return false;
	}
	printf("automate: '%s' made no window in %lu ms\n", name, (unsigned long)ms);
	return true;
}

static bool run_app(const char *name, const char *arg, bool wait,
	bool at, int x, int y) {
	// Its window appears where it belongs rather than being moved
	// there while the app is painting its first frame. Sent BEFORE the
	// launch, for the next new app (tag 0): sent after, a fast app
	// (gpu3d) creates its window first and then jumps.
	if (at)
		z_msg_new_send(wm_pid(), Z_WM_WIN_NEXT_PLACE, 0,
			z_obj_uint32(Z_WM_PACK_PLACE(x, y)));
	if (arg && *arg) z_launch_arg_set(arg);
	uint32_t pid = z_proc_run(name);
	if (!pid) { printf("automate: could not run '%s'\n", name); return true; }
	int slot = app_find(name);
	if (slot < 0) for (slot = 0; slot < APPS_MAX && apps[slot].pid; slot++) ;
	if (slot >= APPS_MAX) slot = 0;
	strncpy(apps[slot].name, name, sizeof(apps[slot].name) - 1);
	apps[slot].pid = pid;
	return wait ? await_app(name, 10000) : true;
}

// Through wm, never z_proc_kill() directly: wm takes the process's
// windows away with it (Z_WM_WIN_KILL). A process killed any other way
// leaves its windows on the screen -- and the kernel hands its pid to
// the next process, which then "owns" them.
static void wm_kill(uint32_t pid) {
	if (!pid || !pid_alive(pid)) return;
	z_msg_new_send(wm_pid(), Z_WM_WIN_KILL, pid, z_obj_none());
	uint32_t end = now() + ms_ticks(1000);
	while (pid_alive(pid) && (int32_t)(now() - end) < 0) {
		z_proc_wait(ms_ticks(20));
		pump();
	}
}

static void kill_app(const char *name) {
	int i = app_find(name);
	if (i < 0) return;
	wm_kill(apps[i].pid);
	apps[i].pid = 0;
}

static void kill_all(void) {
	for (int i = 0; i < APPS_MAX; i++) {
		if (apps[i].pid) wm_kill(apps[i].pid);
		apps[i].pid = 0;
	}
}

static void focus_app(uint32_t pid) {
	z_msg_new_send(wm_pid(), Z_WM_WIN_FOCUS, pid, z_obj_none());
}

static bool close_app(const char *name) {
	uint32_t pid = app_pid(name);
	if (!pid) return true;
	// First what the close icon does -- Z_WM_CLOSE to the app, or for a
	// kills-owner window, the whole job. Every windowed app answers it,
	// including the ones with no key for quitting (info, clock, cal).
	z_msg_new_send(wm_pid(), Z_WM_WIN_TBICON, pid, z_obj_uint32(0));
	{
		uint32_t end = now() + ms_ticks(800);
		while ((int32_t)(now() - end) < 0) {
			if (!pid_alive(pid)) { kill_app(name); return true; }
			if (!wait_ms(40)) return false;
		}
	}
	focus_app(pid);
	if (!wait_ms(60)) return false;
	// Ctrl+Q is the convention (text, view, ...); Escape is what the
	// audio apps quit on. Both are asked for, because an app killed
	// from outside never gets to stop the mixer channels it left
	// running (sw/apps/midi's comment on CLOSE_KILLS_OWNER). Killing is
	// the last resort, for an app asking "save changes?".
	static const uint8_t how[2][2] = {
		{ 0x14, Z_KBD_MOD_LCTRL },		// Ctrl+Q
		{ 0x29, 0 },					// Escape
	};
	for (int k = 0; k < 2; k++) {
		if (!tap(how[k][0], how[k][1], 30)) return false;
		uint32_t end = now() + ms_ticks(700);
		while ((int32_t)(now() - end) < 0) {
			if (!pid_alive(pid)) { kill_app(name); return true; }
			if (!wait_ms(50)) return false;
		}
		focus_app(pid);
	}
	printf("automate: '%s' did not close, killing it\n", name);
	kill_app(name);
	return true;
}

// -- parsing helpers --

static char *skip_ws(char *s) { while (*s == ' ' || *s == '\t') s++; return s; }

// Next whitespace-separated word, NUL-terminated in place.
static char *word(char **s) {
	char *p = skip_ws(*s);
	if (!*p) { *s = p; return NULL; }
	char *w = p;
	if (*p == '"') {
		w = ++p;
		while (*p && *p != '"') p++;
	} else {
		while (*p && *p != ' ' && *p != '\t') p++;
	}
	if (*p) *p++ = 0;
	*s = p;
	return w;
}

// The rest of the line as text: quotes stripped, \n made a newline.
static char *rest(char *s) {
	s = skip_ws(s);
	int n = (int)strlen(s);
	while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
	if (n >= 2 && s[0] == '"' && s[n - 1] == '"') { s[n - 1] = 0; s++; }
	char *r = s, *w = s;
	while (*r) {
		if (r[0] == '\\' && r[1] == 'n') { *w++ = '\n'; r += 2; }
		else *w++ = *r++;
	}
	*w = 0;
	return s;
}

static int num(char **s, int dflt) {
	char *w = word(s);
	return w ? atoi(w) : dflt;
}

static int find_label(const char *name) {
	for (int i = 0; i < nlines; i++) {
		char *l = skip_ws(lines[i]);
		if (!strncmp(l, "label", 5) && (l[5] == ' ' || l[5] == '\t')) {
			char tmp[48];
			strncpy(tmp, skip_ws(l + 5), sizeof(tmp) - 1);
			tmp[sizeof(tmp) - 1] = 0;
			char *e = tmp;
			while (*e && *e != ' ' && *e != '\t') e++;
			*e = 0;
			if (!strcmp(tmp, name)) return i;
		}
	}
	return -1;
}

static bool load_script(const char *path) {
	int n = fs_read_file((char *)path, script_buf, SCRIPT_MAX);
	if (n <= 0) { printf("automate: cannot read %s\n", path); return false; }
	script_buf[n] = 0;
	nlines = 0;
	char *p = script_buf;
	while (*p && nlines < LINES_MAX) {
		lines[nlines++] = p;
		while (*p && *p != '\n') p++;
		if (*p) *p++ = 0;
	}
	for (int i = 0; i < nlines; i++) {
		int l = (int)strlen(lines[i]);
		if (l && lines[i][l - 1] == '\r') lines[i][l - 1] = 0;
	}
	printf("automate: %s, %d lines\n", path, nlines);
	return true;
}

// -- the commands --

static bool cmd_melody(char *s) {
	int tempo = num(&s, 200);
	char *w;
	while ((w = word(&s))) {
		int len = 1;
		char *colon = strchr(w, ':');
		if (colon) { *colon = 0; len = atoi(colon + 1); if (len < 1) len = 1; }
		uint32_t dur = (uint32_t)(tempo * len);
		if (!strcmp(w, "-")) { if (!wait_ms(dur)) return false; continue; }
		uint8_t u, m;
		if (!parse_combo(w, &u, &m)) { printf("automate: melody: '%s'?\n", w); continue; }
		uint32_t hold = dur * 85 / 100;
		inject(u, m, true);
		if (!wait_ms(hold)) { inject(u, m, false); return false; }
		inject(u, m, false);
		if (!wait_ms(dur - hold)) return false;
	}
	return true;
}

static bool xy(char **s, int *x, int *y) {
	char *a = word(s), *b = word(s);
	if (!a || !b) return false;
	*x = atoi(a) + origin_x;
	*y = atoi(b) + origin_y;
	return true;
}

static bool exec_line(char *line) {

	char *s = skip_ws(line);
	if (!*s || *s == '#') return true;

	char *cmd = word(&s);
	for (char *c = cmd; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
	printf("automate: %s %s\n", cmd, s);

	int x, y;

	if (!strcmp(cmd, "say"))            say(rest(s), true);
	else if (!strcmp(cmd, "say&"))      say(rest(s), false);
	else if (!strcmp(cmd, "sync"))      return sync_speech();
	else if (!strcmp(cmd, "narrate") || !strcmp(cmd, "narrate&")) {
		char *t = rest(s);
		char *bar = strstr(t, " | ");
		char *spoken = t;
		if (bar) { *bar = 0; spoken = bar + 3; }
		caption(t);
		// Captions show '\n' as a break; speech reads it as a space.
		for (char *c = spoken; *c; c++) if (*c == '\n') *c = ' ';
		say(spoken, cmd[7] != '&');
	}
	else if (!strcmp(cmd, "caption"))   caption(rest(s));
	else if (!strcmp(cmd, "caption-off")) caption("");
	else if (!strcmp(cmd, "caption-size")) { cap_scale = num(&s, 2); }
	else if (!strcmp(cmd, "caption-pos")) {
		char *w = word(&s);
		cap_pos = !w ? Z_CAPTION_BOTTOM : !strcmp(w, "top") ? Z_CAPTION_TOP :
			!strcmp(w, "center") ? Z_CAPTION_CENTER : Z_CAPTION_BOTTOM;
	}
	else if (!strcmp(cmd, "caption-style")) {
		char *w;
		cap_flags = 0;
		while ((w = word(&s))) {
			if (!strcmp(w, "compact")) cap_flags |= Z_CAPTION_COMPACT;
			else if (!strcmp(w, "inverse")) cap_flags |= Z_CAPTION_INVERSE;
		}
	}
	else if (!strcmp(cmd, "pause"))     return wait_ms((uint32_t)num(&s, 1000));
	else if (!strcmp(cmd, "run") || !strcmp(cmd, "run&")) {
		// run APP [FILE] [at X Y]
		char *name = word(&s);
		char *arg = word(&s);
		bool at = false;
		int ax = 0, ay = 0;
		if (arg && !strcmp(arg, "at")) arg = NULL, at = true;
		else if (arg) { char *a = word(&s); at = a && !strcmp(a, "at"); }
		if (at) { ax = num(&s, 0); ay = num(&s, 0); }
		if (name) return run_app(name, arg, cmd[3] != '&', at, ax, ay);
	}
	else if (!strcmp(cmd, "await")) {
		char *name = word(&s);
		if (name) return await_app(name, (uint32_t)num(&s, 10000));
	}
	else if (!strcmp(cmd, "close"))     { char *n = word(&s); if (n) return close_app(n); }
	else if (!strcmp(cmd, "kill"))      { char *n = word(&s); if (n) kill_app(n); return wait_ms(100); }
	else if (!strcmp(cmd, "killall"))   { kill_all(); return wait_ms(200); }
	else if (!strcmp(cmd, "focus")) {
		char *n = word(&s);
		uint32_t pid = n ? app_pid(n) : 0;
		if (pid) { focus_app(pid); return wait_ms(80); }
	}
	else if (!strcmp(cmd, "place")) {
		char *n = word(&s);
		uint32_t pid = n ? app_pid(n) : 0;
		int ox = origin_x, oy = origin_y;
		origin_x = origin_y = 0;
		bool ok = xy(&s, &x, &y);
		origin_x = ox; origin_y = oy;
		if (pid && ok) {
			z_msg_new_send(wm_pid(), Z_WM_WIN_PLACE, pid, z_obj_uint32(Z_WM_PACK_PLACE(x, y)));
			return wait_ms(80);
		}
	}
	else if (!strcmp(cmd, "origin")) {
		char *n = word(&s);
		origin_x = origin_y = 0;
		if (n && strcmp(n, "screen")) {
			uint32_t pid = app_pid(n);
			if (pid && query(pid) && info.found) { origin_x = info.cx0; origin_y = info.cy0; }
		}
	}
	else if (!strcmp(cmd, "key")) {
		char *w;
		while ((w = word(&s))) {
			uint8_t u, m;
			if (!parse_combo(w, &u, &m)) { printf("automate: key '%s'?\n", w); continue; }
			if (!tap(u, m, 30)) return false;
			if (!wait_ms(40)) return false;
		}
	}
	else if (!strcmp(cmd, "hold")) {
		char *w = word(&s);
		uint8_t u, m;
		if (w && parse_combo(w, &u, &m)) return tap(u, m, (uint32_t)num(&s, 300));
	}
	else if (!strcmp(cmd, "type"))      return type_text(rest(s));
	else if (!strcmp(cmd, "type-speed")) type_ms = (uint32_t)num(&s, 55);
	else if (!strcmp(cmd, "melody"))    return cmd_melody(s);
	else if (!strcmp(cmd, "layout")) {
		char *w = word(&s);
		int id = w ? z_kbd_layout_find(w) : -1;
		if (id < 0) printf("automate: layout '%s'?\n", w ? w : "");
		else { z_kbd_set_active_layout(id); keymap_build(); }
	}
	else if (!strcmp(cmd, "mouse")) {
		if (xy(&s, &x, &y)) return glide(x, y, (uint32_t)num(&s, 400));
	}
	else if (!strcmp(cmd, "click") || !strcmp(cmd, "dclick")) {
		if (xy(&s, &x, &y) && !glide(x, y, 350)) return false;
		if (!click()) return false;
		if (cmd[0] == 'd' && !click()) return false;
	}
	else if (!strcmp(cmd, "press"))     return button(true);
	else if (!strcmp(cmd, "release"))   return button(false);
	else if (!strcmp(cmd, "drag")) {
		int x2, y2;
		if (!xy(&s, &x, &y) || !xy(&s, &x2, &y2)) return true;
		uint32_t ms = (uint32_t)num(&s, 600);
		return glide(x, y, 300) && button(true) && glide(x2, y2, ms) && button(false);
	}
	else if (!strcmp(cmd, "stroke")) {
		// stroke MS x y x y ...: press at the first point, glide through
		// the rest, MS per segment, release.
		uint32_t ms = (uint32_t)num(&s, 200);
		bool first = true;
		while (xy(&s, &x, &y)) {
			if (first) {
				if (!glide(x, y, 250) || !button(true)) return false;
				first = false;
			} else if (!glide(x, y, ms)) { button(false); return false; }
		}
		if (!first) return button(false);
	}
	else if (!strcmp(cmd, "tbicon")) {
		char *n = word(&s), *k = word(&s);
		uint32_t pid = n ? app_pid(n) : 0;
		if (!pid || !k) return true;
		int kind = !strcmp(k, "close") ? 0 : !strcmp(k, "save") ? Z_WM_TBICON_SAVE :
			!strcmp(k, "open") ? Z_WM_TBICON_OPEN : !strcmp(k, "new") ? Z_WM_TBICON_NEW :
			!strcmp(k, "font") ? Z_WM_TBICON_FONT : -1;
		if (kind < 0 || !query(pid) || !info.found) return true;
		for (int i = 0; i < info.n_icons; i++) {
			if (info.icon_kind[i] != kind) continue;
			if (have_ptr) {
				if (!glide(info.icon_x[i] + 4, info.icon_y[i] + 4, 450)) return false;
				return click() && wait_ms(100);
			}
			z_msg_new_send(wm_pid(), Z_WM_WIN_TBICON, pid, z_obj_uint32((uint32_t)kind));
			return wait_ms(100);
		}
	}
	else if (!strcmp(cmd, "pointer")) {
		char *w = word(&s);
		if (w && !strcmp(w, "hide")) ptr_release();
		else ptr_write();
	}
	else if (!strcmp(cmd, "video")) {
		char *w = word(&s);
		uint32_t m = !w ? 0 : !strcmp(w, "amber") ? Z_VIDEO_MODE_AMBER :
			!strcmp(w, "green") ? Z_VIDEO_MODE_GREEN :
			!strcmp(w, "paper") ? Z_VIDEO_MODE_PAPER : Z_VIDEO_MODE_WHITE;
		z_video_set_mode(m);
	}
	else if (!strcmp(cmd, "rate")) {
		rate_wpm = num(&s, 180);
		if (ensure_tts())
			z_msg_new_send(tts_pid, Z_TTS_SET, 0,
				z_obj_uint32(Z_TTS_SET_PACK(Z_TTS_PARAM_RATE, rate_wpm)));
	}
	else if (!strcmp(cmd, "print"))     printf("automate: -- %s\n", rest(s));
	else if (!strcmp(cmd, "voice")) {
		// voice synth|female|recorded: switched silently, so the next
		// sentence is the first thing heard in it.
		char *w = word(&s);
		uint32_t v = !w ? Z_TTS_VOICE_RECORDED :
			(!strcmp(w, "synth") || !strcmp(w, "male")) ? Z_TTS_VOICE_MALE :
			!strcmp(w, "female") ? Z_TTS_VOICE_FEMALE : Z_TTS_VOICE_RECORDED;
		if (!sync_speech()) return false;	// not in the middle of a sentence
		if (v == Z_TTS_VOICE_RECORDED && fs_size("/speech/en.spk") <= 0)
			printf("automate: no speech pack (/speech/en.spk) on this card -- "
				"the recorded voice is in it; see docs/tts_data.md\n");
		if (ensure_tts())
			z_msg_new_send(tts_pid, Z_TTS_SET, 0, z_obj_uint32(
				Z_TTS_SET_PACK(Z_TTS_PARAM_VOICE, v | Z_TTS_VOICE_QUIET)));
		return wait_ms(50);
	}
	else if (!strcmp(cmd, "speech")) {
		char *w = word(&s);
		if (w && !strcmp(w, "off") && tts_pid) {
			z_msg_new_send(tts_pid, Z_TTS_NARRATE, 0, z_obj_uint32(0));
			z_msg_new_send(tts_pid, Z_TTS_QUIT, 0, z_obj_none());
			tts_pid = 0;
		} else ensure_tts();
	}
	else if (!strcmp(cmd, "attract"))   attract_s = num(&s, 60);
	else if (!strcmp(cmd, "interruptible")) {
		char *w = word(&s);
		// Attract mode exists to give way to people; a recording
		// script chained from an in-store one must not switch that off.
		interruptible = attract_s || !(w && !strcmp(w, "off"));
	}
	else if (!strcmp(cmd, "reset")) {
		// Politely first, for the audio apps' sake (close_app()).
		for (int i = 0; i < APPS_MAX; i++)
			if (apps[i].pid && pid_alive(apps[i].pid) && !close_app(apps[i].name))
				return false;
		kill_all();
		caption("");
		z_video_set_mode(Z_VIDEO_MODE_WHITE);
		z_kbd_set_active_layout(0);
		origin_x = origin_y = 0;
		return wait_ms(300);
	}
	else if (!strcmp(cmd, "label"))     ;
	else if (!strcmp(cmd, "chain")) {
		// Carry on in another script, from its top. The copy comes
		// first: load_script() reuses the buffer `w` points into.
		char *w = word(&s);
		if (w) {
			char next[Z_WM_ARG_MAX];
			strncpy(next, w, sizeof(next) - 1);
			next[sizeof(next) - 1] = 0;
			strcpy(script_path, next);
			if (load_script(script_path)) pc = -1;
			else finished = true;
		}
	}
	else if (!strcmp(cmd, "goto")) {
		char *w = word(&s);
		int l = w ? find_label(w) : -1;
		if (l < 0) printf("automate: no label '%s'\n", w ? w : "");
		else pc = l;
	}
	else if (!strcmp(cmd, "loop"))      pc = -1;	// the loop in main() adds one
	else if (!strcmp(cmd, "end"))       finished = true;
	else if (!strcmp(cmd, "stop"))      finished = stopped = true;
	else printf("automate: unknown command '%s'\n", cmd);

	return true;
}

// Leaves the machine to whoever is there: quiet, no caption, pointer
// back to the real mouse.
static void let_go(void) {
	if (tts_pid) {
		z_msg_new_send(tts_pid, Z_TTS_STOP, 0, z_obj_none());
		z_msg_new_send(tts_pid, Z_TTS_NARRATE, 0, z_obj_uint32(0));
	}
	nmarks = 0;
	z_caption_hide();
	if (pbtn) { pbtn = 0; ptr_write(); }
	ptr_release();
}

// Attract mode: after a touch, wait until nobody has touched anything
// for attract_s seconds, then start over.
static void wait_idle(void) {
	uint32_t last = info.real_input;
	uint32_t quiet_since = now();
	printf("automate: waiting for %d s without input\n", attract_s);
	for (;;) {
		z_proc_wait(ms_ticks(500));
		pump();
		if (query(0) && info.real_input != last) {
			last = info.real_input;
			quiet_since = now();
		}
		if ((int32_t)(now() - quiet_since) >= (int32_t)ms_ticks((uint32_t)attract_s * 1000u))
			break;
	}
	real_base = last;
}

int main(void) {

	// One at a time: running it again restarts the demo.
	uint32_t other;
	if (z_pid_lookup("automate0", &other) && other != z_getpid()) {
		printf("automate: stopping the running copy (pid %lu)\n", (unsigned long)other);
		z_proc_kill(other);
		z_proc_wait(ms_ticks(200));
		// Its caption, pointer and narrator lease die with it only in
		// part: clean up after it.
		z_caption_hide();
	}
	char name[24];
	z_pid_register("automate", name, sizeof(name));

	char arg[Z_WM_ARG_MAX];
	arg[0] = 0;
	z_launch_arg_take(arg, sizeof(arg));
	strncpy(script_path, arg[0] ? arg : DEFAULT_SCRIPT, sizeof(script_path) - 1);
	// Which build this is: a card running a stale binary looks exactly
	// like a change that did nothing.
	printf("automate: build %s %s\n", __DATE__, __TIME__);
	if (!load_script(script_path)) return 1;

	ptr_init();
	keymap_build();
	if (query(0)) real_base = info.real_input;
	last_abort_check = now();

	for (;;) {
		aborted = finished = false;
		for (pc = 0; pc < nlines && !finished; pc++) {
			if (!exec_line(lines[pc]) || aborted) break;
		}
		if (aborted) {
			let_go();
			if (!attract_s) break;
			wait_idle();
			load_script(script_path);	// exec_line() tokenised it in place
			continue;
		}
		if (stopped || !attract_s) break;
		load_script(script_path);
	}

	let_go();
	printf("automate: done\n");
	return 0;
}
