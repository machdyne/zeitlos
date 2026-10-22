/*
 * Host test for Z_SYS_PROC_LIST's names (sw/os/procapi.c):
 *
 *   cc -std=gnu99 -Wall -no-pie -I sw/os -I sw/common -o /tmp/test_proclist \
 *      sw/os/tests/test_proclist.c && /tmp/test_proclist
 *
 * A process reports the name it registered, or else the program it was
 * started from -- what wm's dock counts running copies by
 * (docs/dock.md, "Running apps"). procapi.c is included, with the
 * kernel around it played here.
 */

#include <stdio.h>
#include <string.h>

#include "../procapi.c"

volatile z_proc z_procs[Z_PROCS_MAX];
z_obj_t z_ok, z_fail;

static const char *reg[Z_PROCS_MAX];		// the pid registry, played
const char *k_pidreg_name_for(uint32_t pid) { return pid < Z_PROCS_MAX ? reg[pid] : NULL; }
bool k_user_ok_words(const void *p, uint32_t len) { (void)p; (void)len; return true; }

static int checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %d: %s\n", __LINE__, #c); } } while (0)

static z_proc_info_t out[Z_PROCS_MAX];

static const char *name_of(uint32_t pid) {
	z_proc_list_args_t a = { out, Z_PROCS_MAX, 0, 0 };
	k_proc_list((z_obj_t *)&a);
	for (uint32_t i = 0; i < a.count; i++) if (out[i].pid == pid) return out[i].name;
	return NULL;
}

int main(void) {

	memset((void *)z_procs, 0, sizeof(z_procs));

	z_procs[2].base = 0x1000; k_proc_set_exe(2, "text");
	z_procs[3].base = 0x2000; k_proc_set_exe(3, "/apps/text");
	z_procs[4].base = 0x3000; k_proc_set_exe(4, "web"); reg[4] = "web0";
	z_procs[5].base = 0x4000; k_proc_set_exe(5, "/home/me/averyveryverylongprogramname");
	z_procs[6].base = 0x5000;				// not from an executable
	z_procs[7].base = 0x6000; k_proc_set_exe(7, NULL);
	z_procs[8].base = 0x7000; k_proc_set_exe(8, "dir/");

	CHECK(strcmp(name_of(2), "text") == 0);
	CHECK(strcmp(name_of(3), "text") == 0);		// the part after the last '/'
	CHECK(strcmp(name_of(4), "web0") == 0);		// a registered name wins
	CHECK(strcmp(name_of(5), "averyveryverylo") == 0);	// cut to 15
	CHECK(strlen(name_of(5)) == sizeof(z_procs[0].exe) - 1);
	CHECK(strcmp(name_of(6), "") == 0);
	CHECK(strcmp(name_of(7), "") == 0);
	CHECK(strcmp(name_of(8), "") == 0);
	CHECK(name_of(9) == NULL);			// an empty slot is not listed

	k_proc_set_exe(Z_PROCS_MAX, "x");		// out of range: ignored
	k_proc_set_exe(2, "term");			// a slot reused
	CHECK(strcmp(name_of(2), "term") == 0);

	printf("test_proclist: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;

}
