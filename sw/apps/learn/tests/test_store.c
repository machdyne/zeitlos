/* learn -- learners and their records (host). */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../lstore.h"
#include "hostplat.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)
static unsigned char mem[256 << 10];

int main(void) {
	LP = &host_plat;
	snprintf(hp_root, sizeof hp_root, "/tmp/learn_store_card");
	hp_reset_card();
	ls_who_t w[8];
	CHECK(ls_list(w, 8) == 0, "no learners yet");
	CHECK(!ls_name_ok("") && !ls_name_ok("a/b") && !ls_name_ok(" x") && ls_name_ok("Mia") && ls_name_ok("Jonas K"), "names");
	ls_who_t mia, jonas, again;
	CHECK(ls_create("Mia", LS_ON_MACHINE, &mia), "create Mia");
	CHECK(!ls_create("Mia", LS_ON_MACHINE, &again), "Mia twice");
	CHECK(ls_create("Jonas", LS_ON_MACHINE, &jonas), "create Jonas");
	int n = ls_list(w, 8);
	CHECK(n == 2, "two learners: %d", n);
	CHECK(!strcmp(mia.dir, "/data/learn/learners/Mia"), "dir %s", mia.dir);

	CHECK(ls_open(&mia, mem, sizeof mem), "open");
	CHECK(!ls_get("math-3/fractions/comparing/same-denominator"), "nothing yet");
	CHECK(ls_at("math-3/fractions/comparing/same-denominator", 2), "at");
	CHECK(ls_get("math-3/fractions/comparing/same-denominator")->step == 2, "step");
	CHECK(ls_done("math-3/fractions/comparing/same-denominator", 3, 5, 600), "done");
	CHECK(ls_done("math-3/fractions/comparing/same-numerator", 5, 5, 500), "done 2");
	CHECK(ls_report("math-3", 3, "math-3/fractions/comparing/same-numerator", 1, "the answer\nlooks wrong"), "report");
	CHECK(!strcmp(ls_last(), "math-3/fractions/comparing/same-numerator"), "last %s", ls_last());

	// a power cut in the middle of a line: that line is not a record
	{
		char p[700];
		snprintf(p, sizeof p, "%s/data/learn/learners/Mia/progress.txt", hp_root);
		FILE *f = fopen(p, "ab");
		fputs("2026-10-26T10:00 done math-3/broken 1/", f);
		fclose(f);
	}
	CHECK(ls_open(&mia, mem, sizeof mem), "reopen");
	const ls_rec_t *r = ls_get("math-3/fractions/comparing/same-denominator");
	CHECK(r && r->done && r->right == 3 && r->total == 5 && r->step == 0, "kept");
	CHECK(!ls_get("math-3/broken"), "torn line ignored");
	CHECK(!strcmp(ls_last_in("math-3"), "math-3/fractions/comparing/same-numerator"), "last in course");
	CHECK(!strcmp(ls_last_in("sci-3"), ""), "no last in another course");
	// a better score replaces a worse one, a worse one does not
	ls_done("math-3/fractions/comparing/same-denominator", 5, 5, 300);
	ls_done("math-3/fractions/comparing/same-denominator", 1, 5, 300);
	CHECK(ls_get("math-3/fractions/comparing/same-denominator")->right == 5, "best kept");
	{
		char p[700], buf[1024];
		snprintf(p, sizeof p, "%s/data/learn/learners/Mia/reports.txt", hp_root);
		FILE *f = fopen(p, "rb");
		size_t k = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
		if (f) fclose(f);
		buf[k] = 0;
		CHECK(strstr(buf, "math-3 v3") && strstr(buf, "step 2: the answer looks wrong"), "report line: %s", buf);
	}
	// many lessons: the table grows into its memory
	{
		char id[64];
		int ok = 1;
		for (int i = 0; i < 2000; i++) { snprintf(id, sizeof id, "c/u/l%d", i); if (!ls_done(id, 1, 1, 1)) ok = 0; }
		CHECK(ok, "2000 records written");
		CHECK(ls_open(&mia, mem, sizeof mem), "reopen big");
		CHECK(ls_get("c/u/l1999") && ls_get("c/u/l0"), "2000 records read back");
	}
	char st[24];
	ls_stamp(1793000000u, st, sizeof st);
	CHECK(!strcmp(st, "2026-10-26T07:33"), "stamp %s", st);
	ls_stamp(0, st, sizeof st);
	CHECK(!strcmp(st, "-"), "no clock");
	printf("%s: %d failed\n", fails ? "test_store FAILED" : "test_store ok", fails);
	return fails != 0;
}
