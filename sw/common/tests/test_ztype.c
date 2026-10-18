/*
 * Host test for the file type table, sw/common/ztype.c. Run from the
 * top of the tree, because it checks each entry against sw/apps:
 *
 *   cc -std=gnu99 -Wall -I sw/common -o /tmp/test_ztype \
 *      sw/common/tests/test_ztype.c sw/common/ztype.c && /tmp/test_ztype
 *
 * - every app the table names is a program this tree builds: a
 *   sw/apps/<app>/Makefile with `APP = <app>`, so a typo in the table
 *   cannot turn a double-click into "no such program";
 * - no extension is listed twice, whatever its case;
 * - lookups are case-insensitive, take the last dot of the last path
 *   component, and ignore a dot in a directory name.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "zeitlos.h"
#include "ztype.h"

// z_ftype_is_executable() reads the file; nothing here calls it
int fs_open_read(const char *path) { (void)path; return -1; }
int fs_read_chunk(int h, void *buf, int len) { (void)h; (void)buf; (void)len; return -1; }
void fs_close_handle(int h) { (void)h; }

static int failed, run;

#define CHECK(c, ...) do { run++; if (!(c)) { failed++; \
	printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
	printf("\n"); } } while (0)

// Does sw/apps/<app>/Makefile say APP = <app>?
static int app_builds(const char *app) {
	char path[256], line[256];
	snprintf(path, sizeof path, "sw/apps/%s/Makefile", app);
	FILE *f = fopen(path, "r");
	if (!f) return 0;
	int ok = 0;
	while (fgets(line, sizeof line, f)) {
		char name[64];
		if (sscanf(line, " APP = %63s", name) == 1 && !strcmp(name, app)) ok = 1;
	}
	fclose(f);
	return ok;
}

static int ieq(const char *a, const char *b) {
	for (; *a && *b; a++, b++)
		if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return 0;
	return *a == *b;
}

int main(void) {
	int n = 0;
	for (int i = 0; z_ftypes[i].ext; i++, n++) {
		const z_ftype_t *t = &z_ftypes[i];
		CHECK(app_builds(t->app), "%s -> %s: no sw/apps/%s/Makefile with APP = %s",
			t->ext, t->app, t->app, t->app);
		CHECK(t->desc && strlen(t->desc) <= 24, "%s: description missing or long", t->ext);
		for (const char *p = t->ext; *p; p++)
			CHECK(!islower((unsigned char)*p), "%s: extension not uppercase", t->ext);
		for (int j = 0; j < i; j++)
			CHECK(!ieq(z_ftypes[j].ext, t->ext), "%s listed twice", t->ext);
	}
	CHECK(n > 0, "empty table");

	static const struct { const char *path, *app; } cases[] = {
		{ "/home/song.mid",            "midi"  },
		{ "/home/Song.MIDI",           "midi"  },
		{ "/media/audio/a.wav",        "play"  },
		{ "/media/audio/b.Au",         "play"  },
		{ "/media/audio/c.mod",        "track" },
		{ "/home/part.stl",            "gpu3d" },
		{ "/home/pic.svg",             "view"  },
		{ "/home/site/index.html",     "web"   },
		{ "/HOME/PAGE.HTM",            "web"   },
		{ "/home/notes.txt",           "text"  },
		{ "/home/my.dir/readme",       NULL    },	// dot in a directory
		{ "/home/archive.tar.gz",      "convert" },	// last dot: GZ; convert sees .tar.gz
		{ "/home/Report.DOCX",         "convert" },
		{ "/home/book.epub",           "convert" },
		{ "/home/budget.xlsx",         "convert" },
		{ "/home/trailing.",           NULL    },
	};
	for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		const char *got = z_ftype_app_for(cases[i].path);
		CHECK((got == NULL && cases[i].app == NULL) ||
			(got && cases[i].app && !strcmp(got, cases[i].app)),
			"%s: got %s, expected %s", cases[i].path, got ? got : "(none)",
			cases[i].app ? cases[i].app : "(none)");
	}

	printf("%s: %d checks, %d failed (%d types)\n", failed ? "FAIL" : "ok", run, failed, n);
	return failed ? 1 : 0;
}
