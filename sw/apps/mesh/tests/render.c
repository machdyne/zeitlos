/*
 * Render the mesh window to an image, to LOOK AT before it reaches a
 * screen. See sw/common/tests/zrender.h.
 *
 *   make render                  a channel, live, messages and unread
 *   make render WHAT=dm          a direct message, acks, a long line
 *   make render WHAT=log         F2: the node's console
 *   make render WHAT=help        F1
 *   make render WHAT=connecting  the config dump in progress
 *   make render WHAT=noserial    `serial` not running
 *   make render WHAT=small       a window resized small
 *
 * mesh_ui.c, mesh_view.c, mesh_model.c, zwin.c and zrtc.c are the real
 * sources; the pixel plotting is zrender.h's.
 */

#include "../../../common/tests/zrender.h"
#include "../../../common/tests/ztramp.h"

#define MESH_UI_HOST
#include "../mesh_ui.c"

// -- stubs --

uint32_t mesh_ui_host_clock(void) { return 1758800400u; }	// 2026-09-25 11:40 UTC
bool z_caption_show(const char *s, uint32_t o) { (void)s; (void)o; return true; }
bool z_cfg_get(const char *key, char *out, size_t len) {
	(void)key;
	strncpy(out, "CET-1CEST,M3.5.0,M10.5.0/3", len);
	return true;
}

static mesh_model_t model;
static mesh_session_t sess;

#define HB   0x849b6ba0u
#define E314 0xdb29e314u
#define ALI  0x11223344u
#define BOB  0x55667788u
#define JP   0x0badcafeu
#define GR   0x12121212u

static void node(uint32_t num, const char *sn, const char *ln, int hops,
	int snr, int batt, uint32_t heard) {
	mesh_node_t *n = mesh_node_get(&model, num);
	strcpy(n->short_name, sn);
	strcpy(n->long_name, ln);
	n->has_user = 1;
	n->hops_away = (int8_t)hops;
	n->snr_x10 = (int16_t)snr;
	n->battery = (uint8_t)batt;
	n->last_heard = heard;
}

static mesh_msg_t *msg(uint32_t from, uint32_t to, int ch, const char *t,
	int status, uint32_t when) {
	mesh_msg_t *g = mesh_msg_new(&model);
	g->from = from;
	g->to = to;
	g->channel = (uint8_t)ch;
	g->dm = to != MESH_BROADCAST;
	g->status = (uint8_t)status;
	g->rx_time = when;
	g->id = g->seq;
	strcpy(g->text, t);
	g->len = (uint16_t)strlen(t);
	mesh_view_note(&V, &model, g);
	return g;
}

static void scene(void) {
	uint32_t t = 1758800400u;
	mesh_model_init(&model);
	model.my_num = HB;
	strcpy(model.firmware, "2.7.3.cf574c7");
	model.modem_preset = 0;
	model.region = 3;
	model.chan[0].role = CH_ROLE_PRIMARY;
	model.chan[1].role = CH_ROLE_SECONDARY;
	strcpy(model.chan[1].name, "LD");
	node(HB, "6ba0", "Meshtastic 6ba0", 0, MESH_UNKNOWN_SNR, 101, t);
	node(E314, "e314", "Meshtastic e314", 1, 55, 87, t - 120);
	node(ALI, "ALI", "Alice", 0, 92, 64, t - 30);
	node(BOB, "BOB", "Bob the builder, long name", 3, -45, 12, t - 7200);
	node(JP, "\xe6\x9d\xb1", "\xe6\x9d\xb1\xe4\xba\xac\xe3\x83\x8e\xe3\x83\xbc\xe3\x83\x89", 2, 10, 255, t - 900);
	node(GR, "GR\xc3\x9c", "Gr\xc3\xbc\xc3\x9f" "e aus Bamberg", -1, MESH_UNKNOWN_SNR, 255, 0);
	sess.state = MESH_ST_LIVE;
	sess.m = &model;
}

int main(int argc, char **argv) {
	const char *out = argc > 1 ? argv[1] : "/tmp/mesh.pbm";
	const char *what = argc > 2 ? argv[2] : "chat";
	uint32_t t = 1758800400u;
	int w = WIN_W, h = WIN_H;

	if (!strcmp(what, "small")) { w = 300; h = 200; }
	if (!z_tramp_install() || !z_render_open(&win, w, h)) {
		printf("render: skipped (cannot map the fixed addresses)\n");
		return 77;
	}
	M = &model;
	S = &sess;
	mesh_view_init(&V);
	mesh_input_clear(&IN);
	scene();
	tz_ok = z_tz_parse("CET-1CEST,M3.5.0,M10.5.0/3", &tz);
	link = LINK_UP;

	msg(E314, MESH_BROADCAST, 0, "anyone on LongFast near Burgwindheim?", MSG_RX, t - 3000);
	msg(HB, MESH_BROADCAST, 0, "yes -- Zeitlos here, on a Heltec V3 over USB", MSG_SENT, t - 2900);
	msg(JP, MESH_BROADCAST, 0, "\xe3\x81\x93\xe3\x82\x93\xe3\x81\xab\xe3\x81\xa1\xe3\x81\xaf from a node with a Japanese name", MSG_RX, t - 2000);
	msg(GR, MESH_BROADCAST, 0, "Gr\xc3\xbc\xc3\x9f Gott! \xf0\x9f\x98\x80 Emoji draw as the missing-glyph box.", MSG_RX, 12345);
	msg(BOB, MESH_BROADCAST, 0, "This is a long message that has to wrap across several lines of the "
		"message pane, so the continuation indent and the word breaks can be checked by eye.", MSG_RX, t - 600);
	msg(ALI, MESH_BROADCAST, 1, "LD meeting at 8", MSG_RX, t - 500);
	msg(ALI, MESH_BROADCAST, 1, "bring the antenna", MSG_RX, t - 400);
	msg(ALI, HB, 0, "are you getting this?", MSG_RX, t - 300);
	{
		mesh_msg_t *g;
		msg(HB, ALI, 0, "loud and clear", MSG_DELIVERED, t - 250);
		msg(HB, ALI, 0, "sending the map now", MSG_SENT, t - 200);
		g = msg(HB, ALI, 0, "and this one did not make it", MSG_FAILED, t - 100);
		g->err = RT_ERR_MAX_RETRANSMIT;
		msg(HB, ALI, 0, "still going", MSG_SENDING, t - 10);
	}
	mesh_view_build(&V, &model);
	// The primary channel shown; ALI's DM and #LD unread.
	mesh_view_select(&V, 0);
	{
		const char *s = "Gr\xc3\xbc\xc3\x9f" "e \xe2\x82\xac 5 an alle";
		while (*s) {
			const char *p = s;
			mesh_input_insert(&IN, z_utf8_next_z(&p));
			s = p;
		}
	}

	if (!strcmp(what, "dm")) {
		int i;
		for (i = 0; i < V.nconv; i++)
			if (V.conv[i].kind == CONV_DM && V.conv[i].peer == ALI) mesh_view_select(&V, i);
		mesh_input_clear(&IN);
	} else if (!strcmp(what, "log")) {
		mode = MODE_LOG;
		mesh_ui_line("INFO  | 11:40:01 812 [Router] Received text msg from=0xdb29e314, id=0x6a3b...");
		mesh_ui_line("DEBUG | 11:40:01 812 [Router] Add packet record fr=0xdb29e314, id=0x6a3b");
		mesh_ui_line("INFO  | 11:40:02 813 [SerialConsole] Sending to phone");
	} else if (!strcmp(what, "help")) {
		mode = MODE_HELP;
	} else if (!strcmp(what, "connecting")) {
		sess.state = MESH_ST_CONFIG;
	} else if (!strcmp(what, "noserial")) {
		sess.state = MESH_ST_DOWN;
		link = LINK_NO_SERIAL;
		mesh_model_forget_config(&model);
		mesh_view_build(&V, &model);
	}

	relayout();
	mesh_ui_flush();
	z_render_write(out, &win, 2);
	printf("wrote %s (%s)\n", out, what);
	return 0;
}
