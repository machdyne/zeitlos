/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * netsettings -- net.cfg for netx's interfaces, gateway and firewall
 * (docs/netsettings.md, docs/netx.md).
 *
 * A window of settings, one a row. Up and Down choose, Enter (or a
 * click) changes: a switch flips, a choice moves on, an address or a
 * list asks for the new value. Save writes /data/net/net.cfg; Apply
 * saves and starts the network again so it takes the new settings
 * (open connections close). Firewall rules opens /data/net/firewall.cfg
 * in the text editor.
 *
 * Only the keys shown here are touched; everything else in net.cfg --
 * comments, WiFi networks, routes, port forwards, keys this does not
 * know -- stays as it was (cfgfile.c).
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"
#include "../../common/zwm.h"
#include "../../common/zwin.h"
#include "../../common/zwidget.h"
#include "../../common/zdialog.h"
#include "../../common/zfont.h"
#include "../../common/zkbd.h"
#include "../../common/zpaths.h"
#include "../../common/zfsapp.h"
#include "cfgfile.h"

#define WIN_W	360
#define WIN_H	318
#define ROW_H	11
#define TOP	4
#define VAL_X	170

static z_win_t win;
static cf_t cfg;
static bool dirty;
static char status[64] = "";

/* -- the rows -- */

enum { R_HEAD, R_PHY, R_ADDR, R_GW, R_NAT, R_DHCPD, R_ALLOW,
	R_FULL, R_FORWARD, R_DNSFWD, R_DNS };

typedef struct { uint8_t kind, ifx; } row_t;

#define MAX_ROWS 32
static row_t rows[MAX_ROWS];
static int nrows, sel = 1;

static const char *const ifs[3] = { "wan", "lan", "lan2" };
static const char *const if_title[3] = {
	"Upstream (wan)", "Local network (lan)", "Second local network (lan2)",
};

static const char *const phys[] = { "none", "builtin", "builtin2", "usbdev", "usb", "xlink" };
static const char *const phy_text[] = {
	"not used", "Ethernet", "Ethernet B (Klinge)", "USB-C, to a PC", "USB Ethernet adapter",
	"Carrier USB, to a PC",
};
#define NPHY 6

static const char *key(int ifx, const char *k)
{
	static char b[32];
	snprintf(b, sizeof(b), "%s.%s", ifs[ifx], k);
	return b;
}

static bool if_used(int ifx)
{
	char p[8];
	snprintf(p, sizeof(p), "%s.", ifs[ifx]);
	return cf_has_prefix(&cfg, p) != 0;
}

static bool on(const char *k, bool dflt)
{
	const char *v = cf_get(&cfg, k);
	return v ? v[0] != '0' : dflt;
}

static void add(int kind, int ifx)
{
	if (nrows < MAX_ROWS) {
		rows[nrows].kind = (uint8_t)kind;
		rows[nrows].ifx = (uint8_t)ifx;
		nrows++;
	}
}

static void build_rows(void)
{
	int i;
	nrows = 0;
	for (i = 0; i < 3; i++) {
		add(R_HEAD, i);
		add(R_PHY, i);
		if (!if_used(i))
			continue;
		add(R_ADDR, i);
		add(R_GW, i);
		add(R_NAT, i);
		add(R_DHCPD, i);
		add(R_ALLOW, i);
	}
	add(R_HEAD, 3);
	add(R_FULL, 0);
	add(R_FORWARD, 0);
	add(R_DNSFWD, 0);
	add(R_DNS, 0);
	if (sel >= nrows) sel = nrows - 1;
	while (sel > 0 && rows[sel].kind == R_HEAD) sel--;
	if (rows[sel].kind == R_HEAD) sel++;
}

/* a row's label and value */
static void row_text(const row_t *r, const char **label, char *val, int cap)
{
	const char *v;
	int i;
	val[0] = 0;
	switch (r->kind) {
	case R_HEAD:
		*label = r->ifx < 3 ? if_title[r->ifx] : "The network";
		return;
	case R_PHY:
		*label = "  port";
		v = cf_get(&cfg, key(r->ifx, "phy"));
		if (!if_used(r->ifx)) { snprintf(val, cap, "%s", phy_text[0]); return; }
		for (i = 1; i < NPHY; i++)
			if (v && !strcmp(v, phys[i])) { snprintf(val, cap, "%s", phy_text[i]); return; }
		snprintf(val, cap, "the board's choice");
		return;
	case R_ADDR:
		*label = "  address";
		v = cf_get(&cfg, key(r->ifx, "ip"));
		snprintf(val, cap, "%s", v ? v : "from DHCP");
		return;
	case R_GW:
		*label = "  gateway";
		v = cf_get(&cfg, key(r->ifx, "gw"));
		snprintf(val, cap, "%s", v ? v : "-");
		return;
	case R_NAT:
		*label = "  share it (NAT)";
		snprintf(val, cap, "%s", on(key(r->ifx, "nat"), false) ? "on" : "off");
		return;
	case R_DHCPD:
		*label = "  hand out addresses";
		snprintf(val, cap, "%s", on(key(r->ifx, "dhcpd"), false) ? "on" : "off");
		return;
	case R_ALLOW:
		*label = "  let in";
		v = cf_get(&cfg, key(r->ifx, "allow"));
		snprintf(val, cap, "%s", v ? v : "-");
		return;
	case R_FULL:
		*label = "  network program";
		snprintf(val, cap, "%s", on("full", true) ? "netx, if on the card" : "net, in flash");
		return;
	case R_FORWARD:
		*label = "  forward between ports";
		snprintf(val, cap, "%s", on("forward", false) ? "on" : "off (NAT turns it on)");
		return;
	case R_DNSFWD:
		*label = "  answer DNS for the LAN";
		snprintf(val, cap, "%s", on("dnsfwd", true) ? "on" : "off");
		return;
	case R_DNS:
		*label = "  nameserver";
		v = cf_get(&cfg, "dns");
		snprintf(val, cap, "%s", v ? v : "from DHCP");
		return;
	}
	*label = "";
}

/* -- drawing -- */

static z_widget_t btns[4];
static z_widget_set_t wset;
static const char *const btn_label[4] = { "Save", "Apply", "Firewall rules", "Close" };
enum { B_SAVE, B_APPLY, B_RULES, B_CLOSE };

static void draw_row(int i)
{
	const char *label;
	char val[48];
	int y = TOP + i * ROW_H, w = z_win_content_w(&win);
	bool s = (i == sel);
	row_text(&rows[i], &label, val, sizeof(val));
	z_win_fill_rect(&win, 0, y - 1, w, ROW_H, s ? 1 : 0);
	/* text2: draw_text paints its cell's background 0, which on the bar
	   would be black on black (zwin.h, docs/ask_app.md) */
	z_win_draw_text2(&win, 6, y, label, s ? 0 : 1, s ? 1 : 0, &z_font_5x8);
	if (val[0])
		z_win_draw_text2(&win, VAL_X, y, val, s ? 0 : 1, s ? 1 : 0, &z_font_5x8);
}

static void draw_status(void)
{
	int y = z_win_content_h(&win) - 18 - 6 - ROW_H - 2;
	z_win_fill_rect(&win, 0, y, z_win_content_w(&win), ROW_H, 0);
	z_win_draw_text(&win, 6, y, status[0] ? status : (dirty ? "Not saved yet." : ""), 1, &z_font_5x8);
}

static void repaint(void)
{
	int i;
	z_win_clear(&win);
	for (i = 0; i < nrows; i++)
		draw_row(i);
	draw_status();
	z_widget_draw_all(&wset, true);
}

static void say(const char *s)
{
	snprintf(status, sizeof(status), "%s", s);
	draw_status();
}

/* -- the dialogs' view of this window -- */

static void on_msg(z_msg_t *m, void *user)
{
	(void)user;
	if (m->subject == Z_WM_SET_CLIP) {
		z_win_apply_clip(&win, &m->obj);
	} else if (m->subject == Z_WM_REDRAW) {
		z_win_apply_redraw(&win, m->obj.val.uint32);
		repaint();
		z_win_redraw_done(&win);
	} else if (m->subject == Z_WM_WINDOW_MOVED) {
		z_win_parse_rect(&win, &m->obj);
	}
}

static const z_dialog_ctx_t dctx = { &win, on_msg, 0 };

static bool ask(const char *title, const char *msg, const char *init, char *out, int cap)
{
	return z_dialog_prompt_empty_ok(&dctx, title, msg, init ? init : "", out, cap);
}

/* -- changing a row -- */

static void set(const char *k, const char *v)
{
	if (cf_set(&cfg, k, v) != 0)
		say("net.cfg is full: not changed");
	else
		dirty = true;
}

static void unset(const char *k)
{
	cf_unset(&cfg, k);
	dirty = true;
}

static void change(int i)
{
	const row_t *r = &rows[i];
	char v[64];
	const char *cur;
	int k;

	status[0] = 0;
	switch (r->kind) {
	case R_PHY:
		cur = if_used(r->ifx) ? cf_get(&cfg, key(r->ifx, "phy")) : "none";
		for (k = 0; k < NPHY; k++)
			if (cur && !strcmp(cur, phys[k])) break;
		k = (k + 1) % NPHY;	/* the next; "the board's choice" goes to builtin */
		if (k == 0) {
			char p[8];
			snprintf(p, sizeof(p), "%s.", ifs[r->ifx]);
			cf_unset_prefix(&cfg, p);
			dirty = true;
		} else {
			bool was = if_used(r->ifx);
			set(key(r->ifx, "phy"), phys[k]);
			if (!was) {
				/* a starting point: the upstream asks DHCP, a LAN
				 * gets a subnet of its own and hands out addresses */
				if (r->ifx == 0) {
					set(key(0, "dhcp"), "1");
				} else {
					set(key(r->ifx, "ip"), r->ifx == 1 ? "192.168.7.1/24" : "192.168.8.1/24");
					set(key(r->ifx, "dhcpd"), "1");
				}
			}
		}
		break;
	case R_ADDR:
		cur = cf_get(&cfg, key(r->ifx, "ip"));
		if (!ask("Address", "\"dhcp\", or a.b.c.d/len", cur ? cur : "dhcp", v, sizeof(v)))
			break;
		if (!strcmp(v, "dhcp") || !v[0]) {
			unset(key(r->ifx, "ip"));
			unset(key(r->ifx, "mask"));
			set(key(r->ifx, "dhcp"), "1");
		} else if (cf_valid_ip(v, 1) && strchr(v, '/')) {
			set(key(r->ifx, "ip"), v);
			unset(key(r->ifx, "mask"));
			set(key(r->ifx, "dhcp"), "0");
		} else {
			say("Not an address with its /len: not changed");
		}
		break;
	case R_GW:
		cur = cf_get(&cfg, key(r->ifx, "gw"));
		if (!ask("Gateway", "a.b.c.d, or nothing", cur, v, sizeof(v)))
			break;
		if (!v[0] || !strcmp(v, "-")) unset(key(r->ifx, "gw"));
		else if (cf_valid_ip(v, 0)) set(key(r->ifx, "gw"), v);
		else say("Not an address: not changed");
		break;
	case R_NAT:
		if (on(key(r->ifx, "nat"), false)) unset(key(r->ifx, "nat"));
		else set(key(r->ifx, "nat"), "1");
		break;
	case R_DHCPD:
		if (on(key(r->ifx, "dhcpd"), false)) unset(key(r->ifx, "dhcpd"));
		else set(key(r->ifx, "dhcpd"), "1");
		break;
	case R_ALLOW:
		cur = cf_get(&cfg, key(r->ifx, "allow"));
		if (!ask("Let in", "ssh, telnet, http, https,\nN (TCP), udp/N; or nothing", cur, v, sizeof(v)))
			break;
		if (!v[0] || !strcmp(v, "-")) unset(key(r->ifx, "allow"));
		else set(key(r->ifx, "allow"), v);
		break;
	case R_FULL:
		if (on("full", true)) set("full", "0");
		else unset("full");
		break;
	case R_FORWARD:
		if (on("forward", false)) unset("forward");
		else set("forward", "1");
		break;
	case R_DNSFWD:
		if (on("dnsfwd", true)) set("dnsfwd", "0");
		else unset("dnsfwd");
		break;
	case R_DNS:
		cur = cf_get(&cfg, "dns");
		if (!ask("Nameserver", "a.b.c.d, or nothing for DHCP's", cur, v, sizeof(v)))
			break;
		if (!v[0]) unset("dns");
		else if (cf_valid_ip(v, 0)) set("dns", v);
		else say("Not an address: not changed");
		break;
	default:
		break;
	}
	build_rows();
	repaint();
}

/* -- the buttons -- */

static char filebuf[CF_LINES * CF_LINE];

static bool save(void)
{
	int n = cf_save(&cfg, filebuf, sizeof(filebuf));
	if (n < 0) {
		say("Too long to save");
		return false;
	}
	fs_mkdir(Z_DIR_NET_DATA);
	if (fs_write_file(Z_PATH_NET_CONFIG, filebuf, n) != n) {	/* the bytes written; 0 on failure */
		say("Could not write net.cfg");
		return false;
	}
	dirty = false;
	say("Saved. Apply starts the network again with it.");
	return true;
}

static void apply(void)
{
	uint32_t pid;
	int t;
	if (z_dialog_confirm(&dctx, "Apply", "Start the network again?\nOpen connections close.",
			Z_DIALOG_OK_CANCEL) != Z_DIALOG_YES)
		return;
	if (!save())
		return;
	if (z_pid_lookup("net0", &pid)) {
		if (z_proc_kill(pid) != Z_OK) {
			say("Saved; restart the board to apply");
			return;
		}
		for (t = 0; t < 30 && z_pid_lookup("net0", &pid); t++)
			z_proc_wait(Z_TICK_HZ / 10);
	}
	if (!z_proc_run("net"))
		say("Saved, but the network did not start");
	else
		say("Applied: the network is starting again");
}

static const char rules_template[] =
	"# netx's firewall rules (docs/netx.md, \"The firewall\").\n"
	"# The first that matches a packet decides it; before the defaults.\n"
	"#\n"
	"#   allow|deny [in=IF] [out=IF] [to=board|through] [proto=tcp|udp|icmp]\n"
	"#              [src=a.b.c.d[/len]] [dst=a.b.c.d[/len]] [sport=N[-M]] [dport=N[-M]]\n"
	"#\n"
	"# allow in=wan proto=tcp dport=23 src=198.51.100.0/24\n";

static void rules(void)
{
	if (fs_size(Z_PATH_NET_FIREWALL) == 0) {	/* missing (or empty): a template */
		fs_mkdir(Z_DIR_NET_DATA);
		fs_write_file(Z_PATH_NET_FIREWALL, (char *)rules_template, (int)strlen(rules_template));
	}
	z_launch_arg_set(Z_PATH_NET_FIREWALL);
	if (!z_proc_run("text"))
		say("The text editor did not start");
	else
		say("Rules take effect when the network starts again");
}

static bool close_ok(void)
{
	if (!dirty)
		return true;
	return z_dialog_confirm(&dctx, "Close", "Close without saving?", Z_DIALOG_OK_CANCEL) == Z_DIALOG_YES;
}

static bool press(int b)
{
	switch (b) {
	case B_SAVE: save(); break;
	case B_APPLY: apply(); break;
	case B_RULES: rules(); break;
	case B_CLOSE: return close_ok();
	}
	return false;
}

/* -- input -- */

static void move(int d)
{
	int i = sel;
	do {
		i += d;
	} while (i >= 0 && i < nrows && rows[i].kind == R_HEAD);
	if (i < 0 || i >= nrows)
		return;
	int old = sel;
	sel = i;
	draw_row(old);
	draw_row(sel);
}

static bool key_in(uint32_t k, uint8_t mods)
{
	(void)mods;
	if (k == Z_KEY_UP) move(-1);
	else if (k == Z_KEY_DOWN) move(1);
	else if (k == 0x0d || k == ' ') change(sel);
	else if (k == 0x1b) return close_ok();
	else if ((k == 's' || k == 'S') && (mods & Z_KBD_MOD_CTRL)) save();
	return false;
}

static bool mouse_in(uint32_t packed)
{
	int cx, cy;
	uint8_t b = (uint8_t)Z_WM_UNPACK_MOUSE_BUTTONS(packed);
	static uint8_t last;
	bool inside = z_win_mouse_content_xy(&win, packed, &cx, &cy);
	int act;
	if (!inside && wset.pressed < 0) { last = b; return false; }
	act = z_widget_mouse(&wset, cx, cy, b);
	if (act >= 0) { last = b; return press(act); }
	/* a click on a row: choose it, and change it */
	if (inside && (last & 1) && !(b & 1) && wset.pressed < 0) {
		int i = (cy - TOP + 1) / ROW_H;
		if (cy >= TOP - 1 && i >= 0 && i < nrows && rows[i].kind != R_HEAD) {
			int old = sel;
			sel = i;
			draw_row(old);
			change(sel);
		}
	}
	last = b;
	return false;
}

int main(void)
{
	char *text;
	int i, bw = 80, gap = 6, x;

	text = fs_mallocfile(Z_PATH_NET_CONFIG);
	if (cf_load(&cfg, text ? text : "") != 0) {
		printf("netsettings: net.cfg has too many or too long lines for this editor\n");
		return 1;
	}
	if (!text) {
		cf_load(&cfg, "# net.cfg -- written by netsettings (docs/netx.md)\n");
	}

	if (z_win_create_flags(&win, "Network settings", WIN_W, WIN_H, -1, -1,
			Z_WIN_FLAG_CLOSE_ICON) != Z_OK) {
		printf("netsettings: needs the window manager\n");
		return 1;
	}
	{
		char ignored[8];
		z_launch_arg_take(ignored, sizeof(ignored));
	}
	memset(btns, 0, sizeof(btns));
	x = (z_win_content_w(&win) - (4 * bw + 3 * gap)) / 2;
	for (i = 0; i < 4; i++) {
		btns[i].type = Z_WIDGET_BUTTON;
		btns[i].label = btn_label[i];
		btns[i].enabled = true;
		btns[i].w = (int16_t)bw;
		btns[i].h = 18;
		btns[i].x = (int16_t)(x + i * (bw + gap));
		btns[i].y = (int16_t)(z_win_content_h(&win) - 18 - 6);
	}
	z_widget_set_init(&wset, btns, 4, &win);
	build_rows();
	repaint();

	for (;;) {
		z_msg_t m;
		if (z_msg_recv(&m, 0) != Z_OK)
			continue;
		switch (m.subject) {
		case Z_WM_SET_CLIP:
			z_win_apply_clip(&win, &m.obj);
			break;
		case Z_WM_REDRAW:
			if (m.obj.type != Z_UINT32 || z_win_redraw_id(m.obj.val.uint32) != win.id)
				break;
			z_win_apply_redraw(&win, m.obj.val.uint32);
			repaint();
			z_win_redraw_done(&win);
			break;
		case Z_WM_WINDOW_MOVED:
			z_win_parse_rect(&win, &m.obj);
			break;
		case Z_WM_CLOSE:
			if (close_ok()) return 0;
			break;
		case Z_WM_KEY:
			if (m.obj.type == Z_UINT32 && Z_WM_UNPACK_KEY_PRESSED(m.obj.val.uint32) &&
					key_in(Z_WM_UNPACK_KEY_KEYSYM(m.obj.val.uint32),
						(uint8_t)Z_WM_UNPACK_KEY_MODIFIERS(m.obj.val.uint32)))
				return 0;
			break;
		case Z_WM_MOUSE:
			if (m.obj.type == Z_UINT32 && mouse_in(m.obj.val.uint32))
				return 0;
			break;
		default:
			break;
		}
	}
}
