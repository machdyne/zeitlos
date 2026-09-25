/*
 * Host tests for sw/apps/mesh's protocol, model and session layers,
 * against a simulated Meshtastic node. `make test` in sw/apps/mesh.
 *
 * The node here is built from zpb and mesh_pb.h, so on its own it
 * would only prove the code agrees with itself about field numbers.
 * That half was checked separately, once, by decoding these messages
 * and mesh's own ToRadio bytes with the reference protobuf runtime and
 * the published schema (docs/mesh_app.md, "Phase 3": nothing from that
 * check is in the tree). What this file checks is behaviour: the
 * handshake, nonces, acks, reboots, timers -- and what a lossy link
 * does to the model.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "zpb.h"
#include "../mesh_pb.h"
#include "../mesh_frame.h"
#include "../mesh_model.h"
#include "../mesh_proto.h"
#include "../mesh_session.h"

static int run, failed;

#define CHECK(cond, what) do { run++; if (!(cond)) { failed++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, what); } } while (0)

// -- what the session sent, decoded --

static uint8_t sent[8192];
static uint32_t sent_n;
static int refuse_sends;

static int t_send(void *ctx, const uint8_t *p, uint32_t n) {
	(void)ctx;
	if (refuse_sends) return 1;
	if (sent_n + n <= sizeof(sent)) {
		memcpy(sent + sent_n, p, n);
		sent_n += n;
	}
	return 0;
}

// ToRadio frames the session sent, parsed out of `sent`.
static uint8_t tr[16][400];
static uint32_t tr_len[16];
static int ntr, wake_bytes;

static void tr_frame(void *ctx, const uint8_t *pb, uint32_t len) {
	(void)ctx;
	if (ntr < 16) { memcpy(tr[ntr], pb, len); tr_len[ntr] = len; }
	ntr++;
}

static void tr_line(void *ctx, const char *s) { (void)ctx; (void)s; }

static void parse_sent(void) {
	mesh_framer_t fr;
	uint32_t i;
	ntr = 0;
	wake_bytes = 0;
	for (i = 0; i < sent_n && sent[i] == MESH_START2; i++) wake_bytes++;
	mesh_frame_init(&fr, tr_frame, tr_line, NULL);
	mesh_frame_feed(&fr, sent + wake_bytes, sent_n - wake_bytes);
	sent_n = 0;
}

// The first field `num` of a message; its value or -1.
static int64_t field_of(const uint8_t *pb, uint32_t len, uint32_t num, zpb_field_t *out) {
	zpb_rd_t r;
	zpb_field_t f;
	zpb_rd_init(&r, pb, len);
	while (zpb_next(&r, &f))
		if (f.num == num) { if (out) *out = f; return (int64_t)f.v; }
	return -1;
}

// -- events --

static int ev_count[16];
static mesh_msg_t *last_msg;
static char lines[8][200];
static int nlines;

static void t_event(void *ctx, const mesh_ev_t *ev) {
	(void)ctx;
	if (ev->kind >= 0 && ev->kind < 16) ev_count[ev->kind]++;
	if (ev->msg) last_msg = ev->msg;
}

static void t_line(void *ctx, const char *s) {
	(void)ctx;
	if (nlines < 8) snprintf(lines[nlines], sizeof(lines[0]), "%s", s);
	nlines++;
}

static uint32_t rng = 1;
static uint32_t t_rand(void *ctx) {
	(void)ctx;
	rng = rng * 1664525u + 1013904223u;
	return rng;
}

static mesh_model_t M;
static mesh_session_t S;

static void fresh(void) {
	mesh_model_init(&M);
	mesh_session_init(&S, &M);
	S.send = t_send;
	S.event = t_event;
	S.line = t_line;
	S.rand32 = t_rand;
	S.ctx = NULL;
	sent_n = 0;
	refuse_sends = 0;
	memset(ev_count, 0, sizeof(ev_count));
	last_msg = NULL;
	nlines = 0;
}

// -- the simulated node --

#define ME		0xa1b2c3d4u
#define ALICE	0x11223344u
#define BOB		0x55667788u

static uint8_t wire[16384];
static uint32_t wire_n;
// Frame boundaries in `wire`, for the corruption test.
static uint32_t fstart[64], nfr;

static void node_frame(const zpb_wr_t *w) {
	if (nfr < 64) fstart[nfr++] = wire_n;
	wire_n += mesh_frame_wrap(wire + wire_n, sizeof(wire) - wire_n,
		w->start, zpb_wr_len(w));
}

static void node_text(const char *s) {
	memcpy(wire + wire_n, s, strlen(s));
	wire_n += (uint32_t)strlen(s);
}

static void fr_sub(uint32_t field, const zpb_wr_t *sub) {
	uint8_t b[600];
	zpb_wr_t w;
	zpb_wr_init(&w, b, sizeof(b));
	zpb_put_varint(&w, FR_ID, 1);
	zpb_put_msg(&w, field, sub);
	node_frame(&w);
}

static void node_my_info(uint32_t num) {
	uint8_t b[32];
	zpb_wr_t w;
	zpb_wr_init(&w, b, sizeof(b));
	zpb_put_varint(&w, MI_MY_NODE_NUM, num);
	fr_sub(FR_MY_INFO, &w);
}

static void node_info(uint32_t num, const char *sn, const char *ln, int batt) {
	uint8_t ub[128], mb[16], b[256];
	zpb_wr_t u, dm, w;
	zpb_wr_init(&u, ub, sizeof(ub));
	zpb_put_str(&u, US_LONG_NAME, ln);
	zpb_put_str(&u, US_SHORT_NAME, sn);
	zpb_put_varint(&u, US_HW_MODEL, 43);
	zpb_wr_init(&dm, mb, sizeof(mb));
	zpb_put_varint(&dm, DM_BATTERY, (uint32_t)batt);
	zpb_wr_init(&w, b, sizeof(b));
	zpb_put_varint(&w, NI_NUM, num);
	zpb_put_msg(&w, NI_USER, &u);
	zpb_put_float(&w, NI_SNR, 5.5f);
	zpb_put_fixed32(&w, NI_LAST_HEARD, 1758800000u);
	zpb_put_msg(&w, NI_DEVICE_METRICS, &dm);
	zpb_put_varint(&w, NI_HOPS_AWAY, 1);
	fr_sub(FR_NODE_INFO, &w);
}

static void node_channel(int idx, int role, const char *name) {
	uint8_t sb[32], b[64];
	zpb_wr_t s, w;
	zpb_wr_init(&s, sb, sizeof(sb));
	if (name) zpb_put_str(&s, CS_NAME, name);
	zpb_wr_init(&w, b, sizeof(b));
	zpb_put_varint(&w, CH_INDEX, (uint32_t)idx);
	zpb_put_msg(&w, CH_SETTINGS, &s);
	zpb_put_varint(&w, CH_ROLE, (uint32_t)role);
	fr_sub(FR_CHANNEL, &w);
}

static void node_lora(int preset, int hops) {
	uint8_t lb[16], b[32];
	zpb_wr_t l, w;
	zpb_wr_init(&l, lb, sizeof(lb));
	zpb_put_varint(&l, LC_USE_PRESET, 1);
	zpb_put_varint(&l, LC_MODEM_PRESET, (uint32_t)preset);
	zpb_put_varint(&l, LC_REGION, 3);
	zpb_put_varint(&l, LC_HOP_LIMIT, (uint32_t)hops);
	zpb_wr_init(&w, b, sizeof(b));
	zpb_put_msg(&w, CF_LORA, &l);
	fr_sub(FR_CONFIG, &w);
}

static void node_varint(uint32_t field, uint32_t v) {
	uint8_t b[16];
	zpb_wr_t w;
	zpb_wr_init(&w, b, sizeof(b));
	zpb_put_varint(&w, field, v);
	node_frame(&w);
}

static void node_packet(uint32_t from, uint32_t to, int ch, uint32_t id,
	uint32_t port, const void *payload, uint32_t plen, uint32_t request_id) {
	uint8_t db[300], pb[400];
	zpb_wr_t d, p;
	zpb_wr_init(&d, db, sizeof(db));
	zpb_put_varint(&d, DA_PORTNUM, port);
	zpb_put_bytes(&d, DA_PAYLOAD, payload, plen);
	if (request_id) zpb_put_fixed32(&d, DA_REQUEST_ID, request_id);
	zpb_wr_init(&p, pb, sizeof(pb));
	zpb_put_fixed32(&p, MP_FROM, from);
	zpb_put_fixed32(&p, MP_TO, to);
	if (ch) zpb_put_varint(&p, MP_CHANNEL, (uint32_t)ch);
	zpb_put_msg(&p, MP_DECODED, &d);
	zpb_put_fixed32(&p, MP_ID, id);
	zpb_put_fixed32(&p, MP_RX_TIME, 1758800100u);
	zpb_put_float(&p, MP_RX_SNR, -3.5f);
	zpb_put_varint(&p, MP_HOP_LIMIT, 2);
	zpb_put_int32(&p, MP_RX_RSSI, -97);
	zpb_put_varint(&p, MP_HOP_START, 3);
	fr_sub(FR_PACKET, &p);
}

static void node_ack(uint32_t from, uint32_t request_id, uint32_t err) {
	uint8_t rb[8];
	zpb_wr_t r;
	zpb_wr_init(&r, rb, sizeof(rb));
	zpb_put_varint(&r, RT_ERROR, err);
	node_packet(from, ME, 0, t_rand(NULL), PORT_ROUTING, rb, zpb_wr_len(&r), request_id);
}

static void wire_reset(void) { wire_n = 0; nfr = 0; }

static void deliver(void) {
	mesh_session_rx(&S, wire, wire_n);
	wire_reset();
}

// The whole config dump for nonce, with console text around it.
static void node_dump(uint32_t nonce) {
	node_text("INFO  | ??:??:?? 12 [SerialConsole] Want config \xe2\x80\x94 sending\r\n");
	node_my_info(ME);
	node_info(ME, "HB", "Heltec Base", 101);
	node_info(ALICE, "ALI", "Alice", 87);
	node_info(BOB, "BOB", "Bob", 12);
	node_lora(4, 5);
	node_channel(0, CH_ROLE_PRIMARY, NULL);
	node_channel(1, CH_ROLE_SECONDARY, "Friends");
	node_channel(2, CH_ROLE_DISABLED, NULL);
	node_text("DEBUG | ??:??:?? 12 [SerialConsole] config sent\n");
	node_varint(FR_CONFIG_COMPLETE, nonce);
}

// The nonce of the want_config the session just sent; -1 if none.
static int64_t asked_nonce(void) {
	int i;
	parse_sent();
	for (i = 0; i < ntr && i < 16; i++) {
		int64_t v = field_of(tr[i], tr_len[i], TR_WANT_CONFIG, NULL);
		if (v >= 0) return v;
	}
	return -1;
}

// -- tests --

static void test_handshake(void) {
	int64_t n1, n2;

	fresh();
	mesh_session_up(&S, 1000);
	n1 = asked_nonce();
	CHECK(wake_bytes == 32, "32 wake bytes first");
	CHECK(ntr == 1 && n1 > 0, "then want_config with a nonce");
	CHECK(S.state == MESH_ST_CONFIG, "state CONFIG");

	// An answer to some OTHER request does not end this one.
	node_dump((uint32_t)n1 + 1);
	deliver();
	CHECK(S.state == MESH_ST_CONFIG, "wrong nonce: still CONFIG");

	node_dump((uint32_t)n1);
	deliver();
	CHECK(S.state == MESH_ST_LIVE, "right nonce: LIVE");
	CHECK(ev_count[MESH_EV_CONFIG_DONE] == 1, "one CONFIG_DONE event");
	CHECK(M.my_num == ME, "my node number");
	CHECK(mesh_node_count(&M) == 3, "three nodes");
	CHECK(strcmp(mesh_chan_name(&M, 0), "MediumFast") == 0, "unnamed primary named by preset");
	CHECK(strcmp(mesh_chan_name(&M, 1), "Friends") == 0, "named secondary");
	CHECK(M.chan[2].role == CH_ROLE_DISABLED, "disabled channel");
	CHECK(M.hop_limit == 5, "hop limit from config");
	CHECK(nlines == 4 && strstr(lines[0], "Want config \xe2\x80\x94 sending") != NULL,
		"console lines with UTF-8 intact");
	{
		mesh_node_t *a = mesh_node_lookup(&M, "ali");
		CHECK(a && a->num == ALICE, "lookup by short name, any case");
		CHECK(a && a->battery == 87 && a->snr_x10 == 55 && a->hops_away == 1,
			"battery, SNR, hops");
		CHECK(mesh_node_lookup(&M, "!55667788") &&
			mesh_node_lookup(&M, "!55667788")->num == BOB, "lookup by id");
		CHECK(mesh_node_lookup(&M, "Bob") != NULL, "lookup by long name");
		CHECK(mesh_node_lookup(&M, "carol") == NULL, "unknown name");
		CHECK(mesh_node_lookup(&M, "!123456789") == NULL, "nine hex digits");
	}
	CHECK(M.rejected == 0, "nothing rejected");

	// Timeout: nothing for 15 s -> asked again, with a new nonce.
	fresh();
	mesh_session_up(&S, 0);
	n1 = asked_nonce();
	mesh_session_tick(&S, MESH_CONFIG_TIMEOUT_MS - 1);
	CHECK(asked_nonce() < 0, "not before the timeout");
	mesh_session_tick(&S, MESH_CONFIG_TIMEOUT_MS);
	n2 = asked_nonce();
	CHECK(n2 > 0 && n2 != n1, "asked again with a new nonce");
	node_dump((uint32_t)n1);			// the late answer to the first
	deliver();
	CHECK(S.state == MESH_ST_CONFIG, "late answer to the old nonce ignored");
	node_dump((uint32_t)n2);
	deliver();
	CHECK(S.state == MESH_ST_LIVE, "answer to the new one: LIVE");
}

static void go_live(void) {
	int64_t n;
	fresh();
	mesh_session_up(&S, 0);
	n = asked_nonce();
	node_dump((uint32_t)n);
	deliver();
	memset(ev_count, 0, sizeof(ev_count));
}

static void test_receive(void) {
	mesh_msg_t *g;
	char nm[16];

	go_live();
	node_packet(ALICE, MESH_BROADCAST, 1, 0xdeadbeef, PORT_TEXT, "hi all", 6, 0);
	deliver();
	CHECK(ev_count[MESH_EV_TEXT] == 1 && last_msg, "a text event");
	g = last_msg;
	CHECK(g && g->from == ALICE && !g->dm && g->channel == 1, "from, broadcast, channel");
	CHECK(g && strcmp(g->text, "hi all") == 0, "text");
	CHECK(g && g->snr_x10 == -35 && g->rssi == -97, "SNR and ten-byte RSSI");
	CHECK(strcmp(mesh_name(&M, ALICE, nm, sizeof(nm)), "ALI") == 0, "name");

	// Delivered twice (the node's queue replayed): kept once.
	node_packet(ALICE, MESH_BROADCAST, 1, 0xdeadbeef, PORT_TEXT, "hi all", 6, 0);
	deliver();
	CHECK(ev_count[MESH_EV_TEXT] == 1 && M.msg_count == 1, "duplicate dropped");

	// A DM, with a newline and a control character.
	node_packet(BOB, ME, 0, 0x1234, PORT_TEXT, "line1\nline2\x07", 12, 0);
	deliver();
	g = last_msg;
	CHECK(g && g->dm && strcmp(g->text, "line1\nline2 ") == 0,
		"DM: newline kept, BEL made a space");

	// Hearing from an unknown node adds it.
	node_packet(0x99999999, MESH_BROADCAST, 0, 1, PORT_TEXT, "new", 3, 0);
	deliver();
	CHECK(mesh_node_find(&M, 0x99999999) != NULL, "stranger added");
	CHECK(strcmp(mesh_name(&M, 0x99999999, nm, sizeof(nm)), "!99999999") == 0,
		"unnamed node shows its id");
}

static void test_send_and_acks(void) {
	mesh_msg_t *dm, *bc, *bad;
	zpb_field_t f, g;
	zpb_rd_t r;
	int64_t to, id, hops;

	go_live();
	dm = mesh_session_send_text(&S, ALICE, 0, "hello", 5);
	parse_sent();
	CHECK(dm && dm->status == MSG_SENDING && dm->from == ME, "DM recorded as sending");
	CHECK(ntr == 1 && field_of(tr[0], tr_len[0], TR_PACKET, &f) >= 0, "one ToRadio.packet");
	to = field_of(f.ptr, f.len, MP_TO, NULL);
	id = field_of(f.ptr, f.len, MP_ID, NULL);
	hops = field_of(f.ptr, f.len, MP_HOP_LIMIT, NULL);
	CHECK(to == ALICE && dm && id == dm->id, "to and id on the wire");
	CHECK(hops == 5, "hop limit from the node's config");
	CHECK(field_of(f.ptr, f.len, MP_WANT_ACK, NULL) == 1, "want_ack");
	field_of(f.ptr, f.len, MP_DECODED, &g);
	zpb_sub(&r, &g);
	CHECK(field_of(g.ptr, g.len, DA_PORTNUM, NULL) == PORT_TEXT, "text port");

	// Implicit ack from our own node: SENT. Then Alice's: DELIVERED.
	node_ack(ME, dm->id, RT_ERR_NONE);
	deliver();
	CHECK(dm->status == MSG_SENT, "implicit ack: sent");
	node_ack(ALICE, dm->id, RT_ERR_NONE);
	deliver();
	CHECK(dm->status == MSG_DELIVERED, "ack from destination: delivered");
	CHECK(ev_count[MESH_EV_STATUS] == 2, "two status events");
	// A late implicit ack does not demote it.
	node_ack(ME, dm->id, RT_ERR_NONE);
	deliver();
	CHECK(dm->status == MSG_DELIVERED, "stays delivered");

	bc = mesh_session_send_text(&S, MESH_BROADCAST, 1, "all", 3);
	parse_sent();
	CHECK(bc != NULL, "broadcast sent");
	node_ack(ME, bc->id, RT_ERR_NONE);
	deliver();
	CHECK(bc->status == MSG_SENT, "broadcast: sent, never delivered");

	bad = mesh_session_send_text(&S, BOB, 0, "anyone?", 7);
	parse_sent();
	node_ack(ME, bad->id, RT_ERR_MAX_RETRANSMIT);
	deliver();
	CHECK(bad->status == MSG_FAILED && strcmp(mesh_status_name(bad), "no ack") == 0,
		"failure reason");

	// A broadcast nobody repeated: sent, unconfirmed -- not failed. A
	// late implicit ack still upgrades it.
	bc = mesh_session_send_text(&S, MESH_BROADCAST, 0, "quiet", 5);
	parse_sent();
	node_ack(ME, bc->id, RT_ERR_MAX_RETRANSMIT);
	deliver();
	CHECK(bc->status == MSG_UNHEARD &&
		strcmp(mesh_status_name(bc), "sent, no relay heard") == 0,
		"broadcast with no relay: unheard, not failed");
	node_ack(ME, bc->id, RT_ERR_NONE);
	deliver();
	CHECK(bc->status == MSG_SENT, "a late relay upgrades it");
	// Any other error on a broadcast is a failure.
	bc = mesh_session_send_text(&S, MESH_BROADCAST, 0, "hot", 3);
	parse_sent();
	node_ack(ME, bc->id, RT_ERR_DUTY_CYCLE);
	deliver();
	CHECK(bc->status == MSG_FAILED, "duty cycle limit on a broadcast fails");

	// An ack for something we never sent changes nothing.
	node_ack(ALICE, 0x0badf00d, RT_ERR_NONE);
	deliver();
	CHECK(dm->status == MSG_DELIVERED && bc->status == MSG_FAILED, "stray ack ignored");

	// Limits.
	{
		char big[MESH_PAYLOAD_MAX + 2];
		memset(big, 'x', sizeof(big));
		CHECK(mesh_session_send_text(&S, ALICE, 0, big, MESH_PAYLOAD_MAX) != NULL,
			"exactly the maximum goes");
		CHECK(mesh_session_send_text(&S, ALICE, 0, big, MESH_PAYLOAD_MAX + 1) == NULL,
			"one over does not");
		CHECK(mesh_session_send_text(&S, ALICE, 0, big, 0) == NULL, "empty does not");
		CHECK(mesh_session_send_text(&S, ALICE, 8, big, 1) == NULL, "channel 8 does not");
		refuse_sends = 1;
		CHECK(mesh_session_send_text(&S, ALICE, 0, big, 1) == NULL,
			"transport refusing: not recorded");
		refuse_sends = 0;
		parse_sent();
	}

	mesh_session_down(&S);
	CHECK(mesh_session_send_text(&S, ALICE, 0, "x", 1) == NULL, "not while down");
}

static void test_reboot_and_heartbeat(void) {
	int64_t n;
	int i, beats = 0;

	go_live();
	mesh_session_tick(&S, MESH_HEARTBEAT_MS - 1);
	parse_sent();
	CHECK(ntr == 0, "no heartbeat early");
	mesh_session_tick(&S, MESH_HEARTBEAT_MS);
	parse_sent();
	for (i = 0; i < ntr; i++)
		if (field_of(tr[i], tr_len[i], TR_HEARTBEAT, NULL) >= 0) beats++;
	CHECK(beats == 1, "a heartbeat each minute");

	node_varint(FR_REBOOTED, 1);
	deliver();
	CHECK(S.state == MESH_ST_CONFIG, "rebooted: back to CONFIG");
	CHECK(ev_count[MESH_EV_REBOOTED] == 1, "reboot reported");
	n = asked_nonce();
	CHECK(n > 0, "and asked again");
	CHECK(mesh_node_count(&M) == 3 && M.chan[1].role == CH_ROLE_DISABLED,
		"channels forgotten, node list kept for the names in the history");
	node_dump((uint32_t)n);
	deliver();
	CHECK(S.state == MESH_ST_LIVE && mesh_node_count(&M) == 3, "and relearnt");
}

static void test_plausibility(void) {
	uint8_t b[64], p[64];
	zpb_wr_t w, pk;
	mesh_ev_t ev;

	fresh();
	// A known field with the wrong wire type: MeshPacket.from as varint.
	zpb_wr_init(&pk, p, sizeof(p));
	zpb_put_varint(&pk, MP_FROM, 5);
	zpb_wr_init(&w, b, sizeof(b));
	zpb_put_msg(&w, FR_PACKET, &pk);
	CHECK(mesh_proto_rx(&M, b, zpb_wr_len(&w), &ev) == MESH_EV_REJECT, "wrong wire type rejected");
	// Channel index 9.
	zpb_wr_init(&pk, p, sizeof(p));
	zpb_put_varint(&pk, CH_INDEX, 9);
	zpb_wr_init(&w, b, sizeof(b));
	zpb_put_msg(&w, FR_CHANNEL, &pk);
	CHECK(mesh_proto_rx(&M, b, zpb_wr_len(&w), &ev) == MESH_EV_REJECT, "channel 9 rejected");
	// Hop limit 12.
	zpb_wr_init(&pk, p, sizeof(p));
	zpb_put_varint(&pk, MP_HOP_LIMIT, 12);
	zpb_wr_init(&w, b, sizeof(b));
	zpb_put_msg(&w, FR_PACKET, &pk);
	CHECK(mesh_proto_rx(&M, b, zpb_wr_len(&w), &ev) == MESH_EV_REJECT, "hop limit 12 rejected");
	// Positive RSSI.
	zpb_wr_init(&pk, p, sizeof(p));
	zpb_put_int32(&pk, MP_RX_RSSI, 40);
	zpb_wr_init(&w, b, sizeof(b));
	zpb_put_msg(&w, FR_PACKET, &pk);
	CHECK(mesh_proto_rx(&M, b, zpb_wr_len(&w), &ev) == MESH_EV_REJECT, "RSSI +40 rejected");
	CHECK(M.rejected == 4, "all four counted");
	// Unknown top-level fields: fine.
	zpb_wr_init(&w, b, sizeof(b));
	zpb_put_varint(&w, 19, 1);
	zpb_put_bytes(&w, 18, "zz", 2);
	CHECK(mesh_proto_rx(&M, b, zpb_wr_len(&w), &ev) == MESH_EV_NONE, "newer fields skipped");
	CHECK(M.rejected == 4, "and not counted");
}

// One byte lost at every position of a whole config dump plus traffic.
// Nothing may crash (the ASan build is what makes that a real check),
// the model must stay sane, and we count how often a damaged frame was
// believed -- the number docs/mesh_app.md reports.
static void test_lossy_link(void) {
	static uint8_t clean[16384], cut[16384];
	uint32_t n, lose, i;
	uint32_t damaged = 0, rejected = 0, lost = 0, insane = 0;
	uint32_t nonce = 4242;

	// The stream once, clean.
	wire_reset();
	node_dump(nonce);
	for (i = 0; i < 6; i++)
		node_packet(ALICE, MESH_BROADCAST, 0, 100 + i, PORT_TEXT, "hello there", 11, 0);
	memcpy(clean, wire, wire_n);
	n = wire_n;
	wire_reset();

	for (lose = 0; lose < n; lose++) {
		uint32_t m = 0, j;
		for (j = 0; j < n; j++) if (j != lose) cut[m++] = clean[j];
		fresh();
		mesh_session_up(&S, 0);
		S.nonce = nonce;			// as if the node answered our request
		sent_n = 0;
		mesh_session_rx(&S, cut, m);
		rejected += M.rejected;
		// Invariants.
		for (j = 0; j < MESH_MAX_NODES; j++) {
			mesh_node_t *x = &M.node[j];
			if (!x->used) continue;
			if (x->num == 0 || x->num == MESH_BROADCAST) insane++;
			if (x->hops_away > 7) insane++;
			if (x->battery > 101 && x->battery != 255) insane++;
			if (memchr(x->short_name, 0, sizeof(x->short_name)) == NULL) insane++;
		}
		for (j = 0; j < M.msg_count; j++) {
			mesh_msg_t *g = mesh_msg_at(&M, j);
			if (g->channel >= MESH_MAX_CHANNELS || g->len > MESH_PAYLOAD_MAX) insane++;
			if (strcmp(g->text, "hello there") != 0 && g->status == MSG_RX) damaged++;
		}
		// A believed-but-wrong node: a name that is none of ours.
		for (j = 0; j < MESH_MAX_NODES; j++) {
			mesh_node_t *x = &M.node[j];
			if (!x->used || !x->has_user) continue;
			if (strcmp(x->short_name, "HB") && strcmp(x->short_name, "ALI") &&
				strcmp(x->short_name, "BOB")) damaged++;
		}
		if (M.msg_count < 5) lost++;
	}
	CHECK(insane == 0, "the model stays sane with a byte lost anywhere");
	printf("  lossy link: %u positions; %u frames rejected, %u damaged values "
		"believed, %u runs lost 2+ messages\n", n, rejected, damaged, lost);
	// Believed damage should be rare next to caught damage. Not zero:
	// that is the honest limit of a stream with no CRC.
	CHECK(damaged * 10 < n, "damage believed at under 10% of positions");
}

static void test_helpers(void) {
	char out[16];
	CHECK(mesh_f32_x10(0x40c80000u) == 63, "6.25 -> 63 (rounded)");
	CHECK(mesh_f32_x10(0xc0600000u) == -35, "-3.5 -> -35");
	CHECK(mesh_f32_x10(0) == 0, "0");
	CHECK(mesh_f32_x10(0x7f800000u) == 0, "inf -> 0");
	CHECK(mesh_f32_x10(0x4e6e6b28u) == 0x7fffffff, "1e9 clamps");
	CHECK(mesh_f32_x10(0x3dcccccdu) == 1, "0.1 -> 1");

	mesh_str_clean(out, sizeof(out), (const uint8_t *)"a\xff" "b\xe2\x82", 5, 0);
	CHECK(strcmp(out, "a?b??") == 0, "invalid UTF-8 -> ?");
	mesh_str_clean(out, 4, (const uint8_t *)"ab\xe2\x80\x94", 5, 0);
	CHECK(strcmp(out, "ab") == 0, "never splits a character");
	mesh_str_clean(out, sizeof(out), (const uint8_t *)"\xed\xa0\x80", 3, 0);
	CHECK(strcmp(out, "???") == 0, "surrogates rejected");
	mesh_str_clean(out, sizeof(out), (const uint8_t *)"\xc0\xaf", 2, 0);
	CHECK(strcmp(out, "??") == 0, "overlong rejected");

	mesh_node_id(0x0000abcd, out);
	CHECK(strcmp(out, "!0000abcd") == 0, "node id is eight hex digits");

	// Eviction: a full table drops the least recently seen, never us.
	mesh_model_init(&M);
	M.my_num = 1;
	mesh_node_get(&M, 1);
	for (uint32_t k = 2; k < 2 + MESH_MAX_NODES; k++) mesh_node_get(&M, k);
	CHECK(mesh_node_find(&M, 1) != NULL, "our own node never evicted");
	CHECK(mesh_node_find(&M, 2) == NULL, "the oldest other one was");
	CHECK(M.evicted == 1 && mesh_node_count(&M) == MESH_MAX_NODES, "one eviction");

	// Message ring wraps.
	mesh_model_init(&M);
	for (uint32_t k = 0; k < MESH_MAX_MSGS + 5; k++) mesh_msg_new(&M)->id = k + 1;
	CHECK(M.msg_count == MESH_MAX_MSGS, "ring full");
	CHECK(mesh_msg_at(&M, 0)->id == 6, "oldest is the sixth");
	CHECK(mesh_msg_find(&M, 3) == NULL && mesh_msg_find(&M, MESH_MAX_MSGS + 5), "find");
}

int main(void) {
	test_handshake();
	test_receive();
	test_send_and_acks();
	test_reboot_and_heartbeat();
	test_plausibility();
	test_lossy_link();
	test_helpers();
	printf("mesh session: %d checks, %d failed\n", run, failed);
	return failed ? 1 : 0;
}
