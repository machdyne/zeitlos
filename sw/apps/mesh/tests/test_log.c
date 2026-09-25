/*
 * Host tests for sw/apps/mesh/mesh_log.c: the history file's records.
 * `make test` in sw/apps/mesh.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "../mesh_model.h"
#include "../mesh_log.h"

static int run, failed;

#define CHECK(cond, what) do { run++; if (!(cond)) { failed++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, what); } } while (0)

static mesh_model_t A, B;

static mesh_msg_t *mk(mesh_model_t *m, uint32_t from, uint32_t to, int ch,
	uint32_t id, int st, const char *t) {
	mesh_msg_t *g = mesh_msg_new(m);
	g->from = from; g->to = to; g->channel = (uint8_t)ch; g->id = id;
	g->status = (uint8_t)st; g->dm = to != MESH_BROADCAST;
	g->rx_time = 1758800000u + id;
	strcpy(g->text, t);
	g->len = (uint16_t)strlen(t);
	return g;
}

// Write every message of A as records, read them into B.
static char file[65536];
static int flen;

static void save(mesh_model_t *m) {
	uint32_t i;
	flen = 0;
	flen += (int)strlen(strcpy(file, MESH_LOG_HEADER));
	for (i = 0; i < m->msg_count; i++)
		flen += mesh_log_fmt_msg(mesh_msg_at(m, i), file + flen, (int)sizeof(file) - flen);
}

static uint32_t load(mesh_model_t *m, int chunk) {
	mesh_log_rd_t rd;
	int i;
	mesh_model_init(m);
	mesh_log_rd_init(&rd);
	for (i = 0; i < flen; i += chunk)
		mesh_log_feed(&rd, m, file + i, flen - i < chunk ? flen - i : chunk);
	mesh_log_finish(&rd, m);
	CHECK(rd.bad == 0, "no bad lines in a clean file");
	return rd.loaded;
}

static void test_round_trip(void) {
	char rec[MESH_LOG_LINE_MAX + 2];
	mesh_msg_t *g;
	int n;

	mesh_model_init(&A);
	mk(&A, 0xdb29e314, MESH_BROADCAST, 0, 1, MSG_RX, "plain text");
	mk(&A, 0x849b6ba0, 0x11223344, 0, 2, MSG_DELIVERED, "newline\nthere, back\\slash");
	mk(&A, 0x849b6ba0, MESH_BROADCAST, 1, 3, MSG_UNHEARD, "Gr\xc3\xbc\xc3\x9f" "e \xe2\x82\xac \xe6\x97\xa5");
	mk(&A, 0x11223344, 0x849b6ba0, 0, 4, MSG_RX, "");
	g = mk(&A, 0x849b6ba0, 0x11223344, 0, 0xfedcba98, MSG_FAILED, "x");
	g->err = 5;
	save(&A);
	CHECK(load(&B, 7) == 5, "five messages back, in 7-byte chunks");
	for (uint32_t i = 0; i < 5; i++) {
		mesh_msg_t *a = mesh_msg_at(&A, i), *b = mesh_msg_at(&B, i);
		CHECK(a->from == b->from && a->to == b->to && a->channel == b->channel &&
			a->id == b->id && a->status == b->status && a->err == b->err &&
			a->rx_time == b->rx_time && a->dm == b->dm &&
			strcmp(a->text, b->text) == 0, "every field survives");
	}
	CHECK(load(&B, 1) == 5 && load(&B, 100000) == 5, "any chunking");

	// A tab never reaches the model (received text is cleaned, tabs
	// become spaces), but the format escapes one anyway.
	{
		mesh_msg_t t;
		memset(&t, 0, sizeof(t));
		strcpy(t.text, "a\tb");
		n = mesh_log_fmt_msg(&t, rec, sizeof(rec));
		CHECK(n > 0 && strstr(rec, "a\\tb") != NULL, "tab escaped");
	}

	// One record is one line: nothing in the text breaks it.
	n = mesh_log_fmt_msg(mesh_msg_at(&A, 1), rec, sizeof(rec));
	CHECK(n > 0 && memchr(rec, '\n', (size_t)n - 1) == NULL && rec[n - 1] == '\n',
		"newline and tab escaped");

	// A maximal message of backslashes still fits a line.
	{
		mesh_msg_t *w = mesh_msg_new(&A);
		memset(w->text, '\\', MESH_PAYLOAD_MAX);
		w->text[MESH_PAYLOAD_MAX] = 0;
		n = mesh_log_fmt_msg(w, rec, sizeof(rec));
		CHECK(n > 0 && n <= MESH_LOG_LINE_MAX + 1, "worst case fits");
		mesh_model_init(&B);
		rec[n - 1] = 0;
		CHECK(mesh_log_apply(&B, rec) && mesh_msg_at(&B, 0)->len == MESH_PAYLOAD_MAX,
			"and reads back");
	}
}

static void test_status(void) {
	char rec[64];
	mesh_model_init(&A);
	mesh_msg_t *g = mk(&A, 1, 2, 0, 0x77, MSG_SENDING, "hi");
	save(&A);
	g->status = MSG_DELIVERED;
	flen += mesh_log_fmt_status(g, file + flen, (int)sizeof(file) - flen);
	load(&B, 64);
	CHECK(mesh_msg_at(&B, 0)->status == MSG_DELIVERED, "S record applied");

	// Still `sending` when mesh stopped: it never will hear now.
	mesh_model_init(&A);
	mk(&A, 1, 2, 0, 0x78, MSG_SENDING, "lost");
	save(&A);
	load(&B, 64);
	CHECK(mesh_msg_at(&B, 0)->status == MSG_UNHEARD, "sending becomes unheard on load");

	// A status for an unknown id, or for a received message, is ignored.
	mesh_model_init(&B);
	CHECK(!mesh_log_apply(&B, "S\t00000099\t3\t0"), "status for nothing");
	mk(&B, 5, 6, 0, 0x42, MSG_RX, "theirs");
	CHECK(!mesh_log_apply(&B, "S\t00000042\t3\t0") &&
		mesh_msg_at(&B, 0)->status == MSG_RX, "status never touches a received one");
	(void)rec;
}

static void test_bad_lines(void) {
	static const char *bad[] = {
		"",
		"X\t1",
		"M",
		"M\t1\t0000000x\t00000002\t0\t00000003\t0\t0\ttext",	// bad hex
		"M\t1\t00000001\t00000002\t9\t00000003\t0\t0\ttext",	// channel 9
		"M\t1\t00000001\t00000002\t0\t00000003\t7\t0\ttext",	// status 7
		"M\t1\t00000001\t00000002\t0\t00000003\t0\t0",			// no text field
		"M\t99999999999\t00000001\t00000002\t0\t00000003\t0\t0\tx",	// time overflow
		"M\t1\t001\t00000002\t0\t00000003\t0\t0\tx",			// short hex
		"S\t00000001\t3",										// short S
	};
	unsigned i;
	mesh_model_init(&B);
	for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
		CHECK(!mesh_log_apply(&B, bad[i]), bad[i]);
	CHECK(B.msg_count == 0, "nothing added by bad lines");

	// Among good ones, in a stream: skipped and counted, the rest kept.
	{
		mesh_log_rd_t rd;
		const char *s = "# mesh log v1\nM\t1\t00000001\tffffffff\t0\t00000001\t0\t0\tone\n"
			"garbage line\nM\t2\t00000001\tffffffff\t0\t00000002\t0\t0\ttwo\n";
		static char big[MESH_LOG_LINE_MAX + 50];
		mesh_model_init(&B);
		mesh_log_rd_init(&rd);
		mesh_log_feed(&rd, &B, s, (int)strlen(s));
		memset(big, 'y', sizeof(big) - 1);
		big[sizeof(big) - 1] = '\n';
		mesh_log_feed(&rd, &B, big, (int)sizeof(big));
		{
			const char *t = "M\t3\t00000001\tffffffff\t0\t00000003\t0\t0\tthree";
			mesh_log_feed(&rd, &B, t, (int)strlen(t));	// no final newline
		}
		mesh_log_finish(&rd, &B);
		CHECK(rd.loaded == 3, "three good records around the damage");
		CHECK(rd.bad == 2, "a garbage line and an overlong one counted");
		CHECK(!strcmp(mesh_msg_at(&B, 2)->text, "three"), "a last line without its newline is read");
	}

	// Control characters from a hand-edited file are cleaned like radio text.
	mesh_model_init(&B);
	CHECK(mesh_log_apply(&B, "M\t1\t00000001\tffffffff\t0\t00000001\t0\t0\tbell\x07here"),
		"control character accepted");
	CHECK(!strcmp(mesh_msg_at(&B, 0)->text, "bell here"), "and cleaned");
}

int main(void) {
	test_round_trip();
	test_status();
	test_bad_lines();
	printf("mesh log: %d checks, %d failed\n", run, failed);
	return failed ? 1 : 0;
}
