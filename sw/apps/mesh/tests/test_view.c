/*
 * Host tests for sw/apps/mesh/mesh_view.c: conversations and unread
 * counts, word wrap, the input line. `make test` in sw/apps/mesh.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "../mesh_model.h"
#include "../mesh_view.h"
#include "zutf8.h"

static int run, failed;

#define CHECK(cond, what) do { run++; if (!(cond)) { failed++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, what); } } while (0)

static mesh_model_t M;
static mesh_view_t V;

#define ME 100u

static mesh_msg_t *rx(uint32_t from, uint32_t to, int ch, const char *t) {
	mesh_msg_t *g = mesh_msg_new(&M);
	g->from = from;
	g->to = to;
	g->channel = (uint8_t)ch;
	g->dm = to != MESH_BROADCAST;
	g->status = MSG_RX;
	strcpy(g->text, t);
	g->len = (uint16_t)strlen(t);
	mesh_view_note(&V, &M, g);
	return g;
}

static void named(uint32_t num, const char *sn, uint32_t heard) {
	mesh_node_t *n = mesh_node_get(&M, num);
	strcpy(n->short_name, sn);
	n->has_user = 1;
	n->last_heard = heard;
}

static void test_conversations(void) {
	int i;
	mesh_model_init(&M);
	mesh_view_init(&V);
	M.my_num = ME;
	mesh_node_get(&M, ME);

	mesh_view_build(&V, &M);
	CHECK(V.nconv == 1 && V.conv[0].kind == CONV_CHAN, "no channels yet: primary offered");

	M.chan[0].role = CH_ROLE_PRIMARY;
	M.chan[1].role = CH_ROLE_SECONDARY;
	strcpy(M.chan[1].name, "LD");
	named(1, "AAA", 10);
	named(2, "BBB", 30);
	named(3, "CCC", 20);
	mesh_view_build(&V, &M);
	CHECK(V.nconv == 5, "two channels and three nodes, not us");
	CHECK(V.conv[2].peer == 2 && V.conv[3].peer == 3 && V.conv[4].peer == 1,
		"nodes most recently heard first");
	CHECK(mesh_view_cur(&V) == 0, "primary shown");

	rx(1, MESH_BROADCAST, 0, "in the shown channel");
	rx(1, MESH_BROADCAST, 1, "in the other");
	rx(1, MESH_BROADCAST, 1, "again");
	rx(1, ME, 0, "a DM from AAA");
	mesh_view_build(&V, &M);
	CHECK(V.conv[0].unread == 0, "shown conversation stays read");
	CHECK(V.conv[1].unread == 2, "two unread in #LD");
	CHECK(V.conv[2].peer == 1 && V.conv[2].unread == 1,
		"node with a DM moves to the top of the nodes, one unread");
	CHECK(mesh_view_unread_total(&V) == 3, "three unread in all");

	mesh_view_select(&V, 2);
	CHECK(V.cur_kind == CONV_DM && V.cur_peer == 1, "DM shown");
	CHECK(mesh_view_unread_total(&V) == 2, "selecting marks it read");
	// Our own message never counts unread.
	{
		mesh_msg_t *g = mesh_msg_new(&M);
		g->from = ME; g->to = 3; g->dm = 1; g->status = MSG_SENDING;
		mesh_view_note(&V, &M, g);
	}
	mesh_view_build(&V, &M);
	CHECK(mesh_view_unread_total(&V) == 2, "own message is not unread");
	CHECK(V.conv[2].peer == 3, "newest activity first: our DM to CCC");
	CHECK(mesh_view_cur(&V) == 3 && V.conv[3].peer == 1,
		"shown conversation followed by identity, not position");

	// A rebuild after a reconnect refilled the node DB keeps counts.
	mesh_model_forget_node(&M);
	M.my_num = ME;
	M.chan[0].role = CH_ROLE_PRIMARY;
	M.chan[1].role = CH_ROLE_SECONDARY;
	named(1, "AAA", 10);
	mesh_view_build(&V, &M);
	CHECK(V.nconv == 3, "fewer nodes now");
	CHECK(V.conv[1].unread == 2, "#LD unread survives the reconnect");
	CHECK(mesh_view_cur(&V) == 2 && V.cur_peer == 1, "DM with AAA still shown");

	// The shown node disappears: the first entry is shown instead.
	mesh_model_forget_node(&M);
	M.my_num = ME;
	M.chan[0].role = CH_ROLE_PRIMARY;
	mesh_view_build(&V, &M);
	CHECK(mesh_view_cur(&V) == 0 && V.cur_kind == CONV_CHAN, "falls back to the first");

	// Membership.
	{
		mesh_msg_t g;
		memset(&g, 0, sizeof(g));
		g.from = ME; g.to = 7; g.dm = 1;
		V.cur_kind = CONV_DM; V.cur_peer = 7;
		CHECK(mesh_view_in_cur(&V, &M, &g), "our DM to 7 is in the DM with 7");
		g.from = 7; g.to = ME;
		CHECK(mesh_view_in_cur(&V, &M, &g), "and theirs to us");
		g.dm = 0; g.to = MESH_BROADCAST;
		CHECK(!mesh_view_in_cur(&V, &M, &g), "their broadcast is not");
	}

	// DM tracking table full: the least active slot is reused.
	mesh_view_init(&V);
	for (i = 0; i < MESH_DM_TRACK + 3; i++) {
		mesh_msg_t *g = mesh_msg_new(&M);
		g->from = 1000 + (uint32_t)i; g->to = ME; g->dm = 1; g->status = MSG_RX;
		mesh_view_note(&V, &M, g);
	}
	CHECK(mesh_view_unread_total(&V) == MESH_DM_TRACK, "tracking is bounded");
}

static char wl[16][64];
static int nwl;
static void w_emit(void *ctx, const char *p, int len, int line) {
	(void)ctx; (void)line;
	if (nwl < 16) { memcpy(wl[nwl], p, (size_t)len); wl[nwl][len] = 0; }
	nwl++;
}

static void test_wrap(void) {
	nwl = 0;
	CHECK(mesh_wrap("hello world how are you", 11, 0, w_emit, NULL) == 2, "two lines");
	CHECK(!strcmp(wl[0], "hello world") && !strcmp(wl[1], "how are you"),
		"exact fit, then the rest");
	nwl = 0;
	mesh_wrap("hello world how are you", 10, 2, w_emit, NULL);
	CHECK(!strcmp(wl[0], "hello") && !strcmp(wl[1], "world") &&
		!strcmp(wl[2], "how are") && !strcmp(wl[3], "you"), "indented continuation");
	nwl = 0;
	CHECK(mesh_wrap("abcdefghijkl", 5, 0, w_emit, NULL) == 3, "long word broken");
	CHECK(!strcmp(wl[0], "abcde") && !strcmp(wl[2], "kl"), "hard break");
	nwl = 0;
	CHECK(mesh_wrap("a\nb\n", 10, 0, w_emit, NULL) == 3, "newlines, trailing");
	CHECK(!strcmp(wl[0], "a") && !strcmp(wl[1], "b") && wl[2][0] == 0, "empty last line");
	nwl = 0;
	CHECK(mesh_wrap("", 10, 0, w_emit, NULL) == 1, "empty is one line");
	// CJK counts two columns: four ideographs in 7 columns = 3 + 1.
	nwl = 0;
	CHECK(mesh_wrap("\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe6\x96\x87", 7, 0, w_emit, NULL) == 2,
		"wide characters by columns");
	// A wide character in a one-column room still advances.
	nwl = 0;
	CHECK(mesh_wrap("\xe6\x97\xa5\xe6\x9c\xac", 1, 0, w_emit, NULL) == 2, "no infinite loop");
	// UTF-8 never split.
	nwl = 0;
	mesh_wrap("\xc3\xa4\xc3\xb6\xc3\xbc\xc3\x9f", 3, 0, w_emit, NULL);
	CHECK(nwl == 2 && strlen(wl[0]) == 6 && strlen(wl[1]) == 2, "umlauts by column, whole");
}

static void test_input(void) {
	mesh_input_t in;
	int i, s;
	mesh_input_clear(&in);
	mesh_input_insert(&in, 'h');
	mesh_input_insert(&in, 0xe4);		// ä
	mesh_input_insert(&in, 'y');
	CHECK(in.len == 4 && !strcmp(in.buf, "h\xc3\xa4y"), "UTF-8 insert");
	mesh_input_left(&in);
	mesh_input_left(&in);
	CHECK(in.cur == 1, "left over a two-byte character");
	mesh_input_delete(&in);
	CHECK(!strcmp(in.buf, "hy") && in.cur == 1, "delete removes the whole character");
	mesh_input_insert(&in, 0x20ac);		// €
	CHECK(!strcmp(in.buf, "h\xe2\x82\xacy") && in.cur == 4, "insert in the middle");
	mesh_input_backspace(&in);
	CHECK(!strcmp(in.buf, "hy") && in.cur == 1, "backspace too");
	CHECK(!mesh_input_insert(&in, 0x07) && !mesh_input_insert(&in, 0xd800),
		"no controls, no surrogates");
	mesh_input_end(&in);
	mesh_input_insert(&in, ' ');
	mesh_input_insert(&in, 'w');
	mesh_input_insert(&in, 'o');
	mesh_input_word(&in);
	CHECK(!strcmp(in.buf, "hy ") && in.cur == 3, "Ctrl+W deletes a word");
	mesh_input_word(&in);
	CHECK(in.len == 0, "and the one before, over the space");

	mesh_input_clear(&in);
	for (i = 0; i < 300; i++) mesh_input_insert(&in, 0x20ac);	// 3 bytes each
	CHECK(in.len == (MESH_PAYLOAD_MAX / 3) * 3, "stops at the message limit, whole");
	CHECK(!mesh_input_insert(&in, 'x') || in.len <= MESH_PAYLOAD_MAX, "never over");

	mesh_input_clear(&in);
	for (i = 0; i < 50; i++) mesh_input_insert(&in, 'a' + (uint32_t)(i % 26));
	s = mesh_input_scroll(&in, 0, 20);
	CHECK(z_utf8_cols(in.buf + s, (size_t)(in.cur - s)) == 19, "cursor on the last column");
	mesh_input_home(&in);
	CHECK(mesh_input_scroll(&in, s, 20) == 0, "home scrolls back");
	mesh_input_right(&in);
	CHECK(mesh_input_scroll(&in, 0, 20) == 0, "stays put while the cursor is visible");
}

int main(void) {
	test_conversations();
	test_wrap();
	test_input();
	printf("mesh view: %d checks, %d failed\n", run, failed);
	return failed ? 1 : 0;
}
