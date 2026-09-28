/*
 * Host tests for the BBS core (sw/apps/bbs/core) on the Linux platform
 * (linux/plat.c), with a fake clock.
 *
 *   make -C sw/apps/bbs test
 *
 * A UTF-8 caller is sw/common/zvt100.c -- the emulator Zeitlos's own
 * term uses -- so the BBS's screens are checked as they are SEEN: the
 * escape sequences interpreted, the detection probe answered by the
 * terminal itself (vt_take_reply()), and the text read back off the
 * emulated screen. A CP437 caller and a dumb one are raw byte streams.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <malloc.h>
#if defined(__SANITIZE_ADDRESS__)
// in the sanitizer's runtime (its header is not always installed)
size_t __sanitizer_get_current_allocated_bytes(void);
#endif
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <utime.h>
#include <sys/stat.h>

#include "../core/bbs_int.h"
#include "../../../common/zvt100.h"

extern int plat_fake_time, plat_quiet;
extern uint32_t plat_fake_now, plat_fake_ms;

static int checks, fails;
#define CK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %d: ", __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

static char dir[256];

// -- callers --

enum { T_VT, T_CP437, T_DUMB };
typedef struct {
	int node, kind;
	vt_screen_t vt;
	uint8_t raw[65536];
	uint32_t raw_n;
	bool closed;
} caller_t;

static long nul_bytes;      // sent to any caller, ever: must stay 0

static void pump(caller_t *c) {
	for (int round = 0; round < 50; round++) {
		const uint8_t *p;
		uint32_t n = bbs_output(c->node, &p);
		if (!n) break;
		for (uint32_t i = 0; i < n; i++) nul_bytes += p[i] == 0;
		if (c->raw_n + n < sizeof(c->raw)) { memcpy(c->raw + c->raw_n, p, n); c->raw_n += n; }
		if (c->kind == T_VT) vt_feed(&c->vt, p, n);
		bbs_consumed(c->node, n);
		if (c->kind == T_VT) {
			uint8_t r[64];
			uint32_t rn = vt_take_reply(&c->vt, r, sizeof(r));
			if (rn) bbs_input(c->node, r, rn);
		}
	}
	if (bbs_wants_close(c->node)) { bbs_hangup(c->node); c->closed = true; }
}

static void tick(uint32_t ms) {
	plat_fake_ms += ms;
	plat_fake_now += ms / 1000;
	bbs_poll();
}

static int call(caller_t *c, int kind, const char *transport, const char *user) {
	memset(c, 0, sizeof(*c));
	c->kind = kind;
	vt_init(&c->vt);
	bbs_conn_t w = { transport, "192.0.2.7", user };
	c->node = bbs_connect(&w);
	if (c->node < 0) return -1;
	pump(c);
	if (kind == T_CP437) {
		// an 8-bit terminal: the three bytes of ─ took three columns
		bbs_input(c->node, (const uint8_t *)"\x1b[1;4R", 6);
		pump(c);
		bbs_input(c->node, (const uint8_t *)"\x1b[24;80R", 8);
		pump(c);
	} else if (kind == T_DUMB) {
		tick(3000);
		pump(c);
	}
	pump(c);
	return c->node;
}

static void type(caller_t *c, const char *s) {
	bbs_input(c->node, (const uint8_t *)s, (uint32_t)strlen(s));
	pump(c);
}

// The emulated screen as UTF-8 text, rows joined by newlines.
static char scr[VT_ROWS * (VT_COLS * 4 + 1) + 1];
static const char *screen(caller_t *c) {
	size_t o = 0;
	for (int r = 0; r < VT_ROWS; r++) {
		for (int col = 0; col < VT_COLS; col++) {
			uint32_t cp = vt_glyph_cp((uint8_t)c->vt.cells[r][col].ch);
			if (!cp) continue;
			char b[4];
			int k = utf8_put(cp, b);
			memcpy(scr + o, b, (size_t)k);
			o += (size_t)k;
		}
		scr[o++] = '\n';
	}
	scr[o] = 0;
	return scr;
}

static bool on_screen(caller_t *c, const char *s) {
	return strstr(screen(c), s) != NULL;
}

static bool raw_has(caller_t *c, const void *s, uint32_t n) {
	for (uint32_t i = 0; i + n <= c->raw_n; i++) if (!memcmp(c->raw + i, s, n)) return true;
	return false;
}

static void write_file(const char *rel, const char *text, uint32_t mtime) {
	char p[512];
	snprintf(p, sizeof(p), "%s/%s", dir, rel);
	FILE *f = fopen(p, "w");
	fputs(text, f);
	fclose(f);
	if (mtime) { struct utimbuf t = { mtime, mtime }; utime(p, &t); }
}

static void configure(const char *cfg) {
	write_file("bbs.cfg", cfg, 0);
	bbs_init(dir);
}

// Bytes of heap in use: AddressSanitizer's count when it is in (it
// replaces malloc, so glibc's mallinfo2() sees nothing), else glibc's.
static size_t heap_in_use(void) {
#if defined(__SANITIZE_ADDRESS__)
	return __sanitizer_get_current_allocated_bytes();
#else
	return mallinfo2().uordblks;
#endif
}

// The line editor, chosen in the profile as a caller would (P, E, Q):
// the tests of it were written for it. The full-screen one (zetta) has
// its own, further on. Idempotent -- the key toggles.
static void line_editor(caller_t *c) {
	if (c->node < 0 || !bbs_node[c->node]->logged_in || bbs_node[c->node]->state != N_MENU) return;
	if (bbs_node[c->node]->user.flags & USER_F_LINE_EDITOR) return;
	type(c, "p"); type(c, "e"); type(c, "q");
}

// A new account, all the way to the menu, answering whatever bulletins
// there are.
static void join(caller_t *c, const char *handle, const char *pw, const char *loc) {
	type(c, "new\r");
	char b[128];
	snprintf(b, sizeof(b), "%s\r", handle); type(c, b);
	snprintf(b, sizeof(b), "%s\r", pw); type(c, b);
	type(c, b);
	snprintf(b, sizeof(b), "%s\r", loc); type(c, b);
	type(c, "y");
	for (int i = 0; i < 20 && bbs_node[c->node]->state == N_PAGER; i++) type(c, "\r");
	line_editor(c);
}

static void login(caller_t *c, const char *handle, const char *pw) {
	char b[128];
	snprintf(b, sizeof(b), "%s\r", handle); type(c, b);
	snprintf(b, sizeof(b), "%s\r", pw); type(c, b);
	for (int i = 0; i < 20 && bbs_node[c->node]->state == N_PAGER; i++) type(c, "\r");
	line_editor(c);
}

static void hang(caller_t *c) {
	if (!c->closed) bbs_hangup(c->node);
	c->closed = true;
}

int main(void) {
	static caller_t a, b, x;
	plat_fake_time = 1;
	plat_fake_now = 1790000000;         // 2026-09-21
	plat_fake_ms = 5000;
	plat_quiet = getenv("BBS_TEST_LOG") ? 0 : 1;

	snprintf(dir, sizeof(dir), "/tmp/bbs-test-%d", (int)getpid());
	CK(bbs_init(dir), "init makes the data directory");
	{
		struct stat st;
		char p[300];
		snprintf(p, sizeof(p), "%s/bulletins", dir);
		CK(stat(p, &st) == 0 && S_ISDIR(st.st_mode), "... and bulletins/");
	}

	// -- 1. configuration --
	{
		bbs_cfg_t c;
		cfg_defaults(&c);
		const char *t = "# a comment\nname: Board #1   # the name\r\nnodes: 99\nnew_users: no\n"
			"idle_minutes: 0\nsysop: Phil\nbogus: 1\n";
		cfg_parse(&c, t, (uint32_t)strlen(t));
		CK(!strcmp(c.name, "Board #1") && c.nodes == BBS_NODES_MAX && !c.new_users &&
			c.idle_minutes == 1 && !strcmp(c.sysop, "Phil"),
			"bbs.cfg: comments, a # inside a value, CRLF, clamping (%s)", c.name);
	}
	configure("name: Test BBS\nsysop: Phil\nnodes: 3\npw_iterations: 50\n");
	CK(bbs_nodes() == 3, "three nodes");

	// -- 2. users: the record, passwords, handles --
	{
		user_t u, v;
		uint8_t r[USER_REC_SIZE];
		memset(&u, 0, sizeof(u));
		u.id = 7; strcpy(u.handle, "J\xC3\xBCrgen"); strcpy(u.location, "Berlin");
		u.flags = USER_F_SYSOP; u.level = 255; u.charset = CS_CP437; u.color = COLOR_OFF; u.rows = 30;
		u.created = 111; u.last_login = 222; u.prev_login = 200; u.calls = 9;
		pw_set(&u, "secret!", 50);
		user_pack(&u, r);
		CK(!memcmp(r, "ZBU1", 4) && r[4] == 7 && r[122] == 255, "packed little-endian, with its magic");
		CK(user_unpack(&v, r) && !memcmp(&u, &v, sizeof(u)), "a record round-trips exactly");
		r[0] = 'X';
		CK(!user_unpack(&v, r), "a record without its magic is refused");
		CK(pw_check(&u, "secret!") && !pw_check(&u, "secret") && !pw_check(&u, ""), "passwords");
		user_t w = u;
		pw_set(&w, "secret!", 50);
		CK(memcmp(w.salt, u.salt, 16) && memcmp(w.hash, u.hash, 32), "the same password, salted apart");
		w.pw_iter = 0;
		CK(!pw_check(&w, "secret!"), "no password set: nothing matches");
		CK(!handle_problem("Phil") && !handle_problem("J\xC3\xBCrgen") && !handle_problem("a b-c_d.e"),
			"good handles");
		CK(handle_problem("x") && handle_problem(" x") && handle_problem("a  b") && handle_problem("a@b") &&
			handle_problem("a|b") && handle_problem("NEW") && handle_problem("a\x1b" "b") &&
			handle_problem("abcdefghijklmnopqrstu") && handle_problem("a\xff"),
			"bad handles: short, spaces, @, |, reserved, controls, too long, not UTF-8");
	}

	// -- 3. detection: a UTF-8 ANSI terminal answers the probe by itself --
	call(&a, T_VT, "ssh", "phil");
	{
		node_t *n = bbs_node[a.node];
		CK(n->ansi && n->charset == CS_UTF8 && n->rows == VT_ROWS && n->cols == VT_COLS,
			"UTF-8 and the screen's size, from two answers (%d x %d)", n->rows, n->cols);
		CK(on_screen(&a, "Welcome to Test BBS") && on_screen(&a, "Your sysop is Phil") &&
			on_screen(&a, "ANSI, UTF-8"), "the welcome screen");
		CK(on_screen(&a, "Test BBS \xE2\x94\x82 Welcome"), "the title bar, name and title divided by a line");
	CK(!strstr(screen(&a), "\xEF\xBF\xBD"), "nothing on the screen that term cannot draw (no U+FFFD)");
		CK(!strstr(screen(&a), "[6n") && !strstr(screen(&a), "\xE2\x94\x80\x1b"), "the probe left nothing on the screen");
		CK(on_screen(&a, "Handle (or NEW to join): phil"), "SSH's user name offered at the prompt");
	}
	type(&a, "\x15");                    // Ctrl-U: clear it
	CK(bbs_node[a.node]->field[0] == 0, "Ctrl-U clears the line");

	// -- 4. the first account is the sysop's --
	write_file("bulletins/01-rules.txt", "|15Be kind.|07\nThat is all.\n", plat_fake_now - 100);
	write_file("bulletins/02-news.txt", "The BBS is |RVopen|RO.\n", plat_fake_now - 50);
	write_file("bulletins/notes.bak", "not a bulletin\n", 0);
	type(&a, "new\r");
	CK(on_screen(&a, "Joining"), "NEW: joining");
	type(&a, "x\r");
	CK(on_screen(&a, "at least 2 characters"), "a bad handle is explained");
	type(&a, "Phil\r");
	CK(on_screen(&a, "Password"), "then a password");
	type(&a, "abc\r");
	CK(on_screen(&a, "At least 6"), "too short");
	type(&a, "hunter22\r");
	CK(!on_screen(&a, "hunter22") && on_screen(&a, "********"), "not echoed: stars");
	type(&a, "hunter2x\r");
	CK(on_screen(&a, "not the same"), "two different passwords: asked again");
	type(&a, "hunter22\rhunter22\r");
	type(&a, "Berlin\r");
	CK(on_screen(&a, "Handle: Phil") && on_screen(&a, "Where:  Berlin") && on_screen(&a, "[Y/n]"),
		"a summary before it is made");
	type(&a, "y");
	CK(users_count() == 1, "the account is in users.dat");
	{
		user_t u;
		CK(users_read(0, &u) && u.id == 1 && (u.flags & USER_F_SYSOP) && u.level == LEVEL_SYSOP &&
			u.calls == 1 && u.created == plat_fake_now && u.pw_iter == 50, "the first user is the sysop");
	}
	CK(bbs_node[a.node]->state == N_PAGER && on_screen(&a, "rules") && on_screen(&a, "Be kind."),
		"then the bulletins, all of them the first time, titled without their number");
	CK(!strstr(screen(&a), "|15"), "pipe codes interpreted, not shown");
	type(&a, "\r");
	CK(on_screen(&a, "news") && on_screen(&a, "The BBS is open."), "the second bulletin");
	CK(a.vt.cells[2][11].reverse, "|RV: reverse video");
	type(&a, "\r");
	CK(bbs_node[a.node]->state == N_MENU && on_screen(&a, "Main menu") && on_screen(&a, "[S] Sysop") &&
		on_screen(&a, "Hello, Phil"), "the menu, with the sysop's item");

	// -- 5. a second caller, a second user; who is online --
	call(&b, T_VT, "telnet", "");
	CK(on_screen(&b, "(not encrypted)"), "telnet is marked as unencrypted");
	CK(on_screen(&b, "Handle (or NEW to join): \n") || on_screen(&b, "Handle (or NEW to join):"),
		"no SSH user: an empty prompt");
	join(&b, "J\xC3\xBCrgen", "passwort1", "M\xC3\xBCnchen");
	{
		user_t u;
		CK(users_count() == 2 && users_read(1, &u) && !(u.flags & USER_F_SYSOP) && u.level == 10,
			"a second user is not the sysop");
	}
	CK(!on_screen(&b, "[S] Sysop"), "nor sees the sysop's item");
	type(&b, "w");
	CK(on_screen(&b, "Who is online") && on_screen(&b, "Phil") && on_screen(&b, "J\xC3\xBCrgen") &&
		on_screen(&b, "(you)") && on_screen(&b, "telnet"), "who is online: both, and which is you");
	type(&b, "x");
	type(&b, "u");
	CK(on_screen(&b, "Users") && on_screen(&b, "(sysop)") && on_screen(&b, "M\xC3\xBCnchen"),
		"the user list");
	{
		// the columns line up with a two-byte character in a handle
		int c1 = -1, c2 = -1;
		for (int r = 0; r < VT_ROWS; r++)
			for (int col = 0; col + 5 < VT_COLS; col++) {
				if (b.vt.cells[r][col].ch == 'B' && b.vt.cells[r][col + 1].ch == 'e') c1 = col;
				if (b.vt.cells[r][col].ch == 'M' && (uint8_t)b.vt.cells[r][col + 1].ch == 0xFC) c2 = col;
			}
		CK(c1 > 0 && c1 == c2, "locations line up in one column (%d, %d) despite ü", c1, c2);
	}
	type(&b, "x");

	// -- 6. injection: a caller's text never reaches anyone as codes --
	type(&b, "p");
	type(&b, "l");
	type(&b, "\x15|CL\x1b[2Jevil\r");
	{
		user_t u;
		users_read(1, &u);
		CK(!strcmp(u.location, "|CLevil"), "ESC sequences typed into a field are dropped (%s)", u.location);
	}
	type(&b, "l");
	type(&b, "\x15|CL\x07\x0b\x01" "evil\r");
	{
		user_t u;
		users_read(1, &u);
		CK(!strcmp(u.location, "|CLevil"), "nor are bare control characters kept (BEL, VT, ^A)");
	}
	type(&b, "q");
	type(&a, "u");
	CK(on_screen(&a, "|CLevil") && on_screen(&a, "Users"),
		"a | in someone's location is shown as text, never run as a code");
	type(&a, "x");

	// -- 7. the profile: characters and colour --
	type(&b, "p");
	CK(on_screen(&b, "Profile and terminal") && on_screen(&b, "\xE2\x94\x8C\xE2\x94\x80\xE2\x94\x80\xE2\x94\x90"),
		"the profile, with its box test");
	type(&b, "c");      // detect -> UTF-8
	type(&b, "c");      // -> CP437
	type(&b, "c");      // -> ASCII
	b.raw_n = 0;
	type(&b, "q");
	type(&b, "p");
	CK(on_screen(&b, "+--+") && !raw_has(&b, "\xE2\x94", 2), "ASCII: boxes as +--+, no UTF-8 at all");
	type(&b, "c");      // -> detect
	type(&b, "o");      // colour: detect -> on
	type(&b, "o");      // -> off
	b.raw_n = 0;
	type(&b, "q");
	CK(!raw_has(&b, "\x1b[1;37m", 7) && !raw_has(&b, "\x1b[22;37m", 8) && raw_has(&b, "\x1b[7m", 4),
		"colour off: no colour codes, reverse video kept");
	type(&b, "p"); type(&b, "o"); type(&b, "q");    // back to detect

	// -- 8. a CP437 caller --
	type(&b, "g");
	CK(b.closed && on_screen(&b, "Goodbye"), "goodbye closes the call");
	call(&x, T_CP437, "telnet", "");
	CK(bbs_node[x.node]->charset == CS_CP437 && bbs_node[x.node]->rows == 24, "CP437 detected from three columns");
	CK(raw_has(&x, "ANSI, CP437", 11), "... and said so");
	x.raw_n = 0;
	type(&x, "J\x81rgen\r");            // ü in CP437
	CK(bbs_node[x.node]->state == N_LOGIN_PASS, "a CP437 caller's handle is found (\\x81 is ü)");
	type(&x, "passwort1\r");
	CK(bbs_node[x.node]->logged_in, "and logs in");
	CK(raw_has(&x, "\xC4\xC4\xC4", 3) || raw_has(&x, "Main menu", 9), "(the menu)");
	x.raw_n = 0;
	type(&x, "p");
	CK(raw_has(&x, "\xDA\xC4\xC4\xBF", 4) && raw_has(&x, "\xC9\xCD\xCD\xBB", 4) &&
		raw_has(&x, "\xB0\xB1\xB2\xDB", 4) && !raw_has(&x, "\xE2\x94", 2),
		"the box test in CP437 bytes: single, double, shades");
	type(&x, "q");
	type(&x, "g");

	// -- 9. a dumb terminal: no ANSI --
	call(&x, T_DUMB, "telnet", "");
	CK(!bbs_node[x.node]->ansi && bbs_node[x.node]->charset == CS_ASCII, "no answer: plain text");
	{
		uint32_t start = x.raw_n;
		(void)start;
		x.raw_n = 0;
		type(&x, "Phil\rhunter22\r");
		for (int i = 0; i < 5 && bbs_node[x.node]->state == N_PAGER; i++) type(&x, "\r");
		CK(bbs_node[x.node]->logged_in && !raw_has(&x, "\x1b", 1), "and not one escape sequence after the probe");
		CK(raw_has(&x, "[B] Bulletins", 13), "menu keys in brackets, without colour");
		type(&x, "g");
	}

	// -- 10. wrong passwords, a disabled user --
	call(&x, T_VT, "telnet", "");
	type(&x, "Phil\rnope\r");
	CK(on_screen(&x, "Wrong password") && !x.closed, "a wrong password");
	type(&x, "Phil\rnope\rPhil\rnope\r");
	CK(x.closed && on_screen(&x, "Too many tries"), "three and the call ends");
	call(&x, T_VT, "telnet", "");
	type(&x, "nobody\r");
	CK(on_screen(&x, "No such user"), "an unknown handle");
	hang(&x);

	// the sysop disables Jürgen
	type(&a, "s");
	CK(on_screen(&a, "Sysop") && on_screen(&a, "2 users"), "the sysop menu");
	type(&a, "u");
	type(&a, "j\xC3\xBCrgen\r");
	CK(on_screen(&a, "#2") && on_screen(&a, "active"), "a user found without case");
	type(&a, "d");
	CK(on_screen(&a, "disabled"), "disabled");
	call(&x, T_VT, "telnet", "");
	type(&x, "J\xC3\xBCrgen\rpasswort1\r");
	CK(x.closed && on_screen(&x, "This account is disabled"), "a disabled user cannot log in");
	type(&a, "d");
	CK(on_screen(&a, "active"), "enabled again");
	type(&a, "l");
	type(&a, "42\r");
	{ user_t u; users_read(1, &u); CK(u.level == 42, "a level set"); }
	type(&a, "p");
	type(&a, "newpass9\r");
	call(&x, T_VT, "telnet", "");
	type(&x, "J\xC3\xBCrgen\rnewpass9\r");
	for (int i = 0; i < 5 && bbs_node[x.node]->state == N_PAGER; i++) type(&x, "\r");
	CK(bbs_node[x.node]->logged_in, "the sysop's new password works");
	CK(!on_screen(&x, "rules"), "bulletins seen before are not shown again");
	type(&a, "q");
	type(&a, "k");
	char kb[16];
	snprintf(kb, sizeof(kb), "%d\r", x.node + 1);
	type(&a, kb);
	pump(&x);
	CK(x.closed && on_screen(&x, "sysop has ended this call"), "the sysop disconnects a node");
	type(&a, "q");

	// -- 11. a new bulletin is shown at the next call --
	write_file("bulletins/03-later.txt", "Fresh news.\n", plat_fake_now + 10);
	tick(20000);
	call(&x, T_VT, "telnet", "");
	type(&x, "J\xC3\xBCrgen\rnewpass9\r");
	CK(bbs_node[x.node]->state == N_PAGER && on_screen(&x, "Fresh news.") && !on_screen(&x, "Be kind"),
		"only the bulletin newer than the last call");
	type(&x, "\r");
	CK(bbs_node[x.node]->state == N_MENU, "then the menu");

	// -- 12. a long bulletin pages; Q stops --
	{
		static char big[8000];
		size_t o = 0;
		for (int i = 1; i <= 100; i++) o += (size_t)snprintf(big + o, sizeof(big) - o, "line %d of a long one\n", i);
		write_file("bulletins/04-long.txt", big, plat_fake_now + 20);
	}
	type(&x, "b");
	for (int i = 0; i < 4 && !on_screen(&x, "line 1 of"); i++) type(&x, "\r");
	CK(on_screen(&x, "line 1 of") && on_screen(&x, "Enter to go on"), "a page and a prompt");
	type(&x, "\r");
	CK(on_screen(&x, "line 30 of"), "the next page");
	type(&x, "q");
	CK(bbs_node[x.node]->state == N_MENU, "Q stops at the menu");

	// -- 13. ANSI art: CP437 with its escape sequences --
	write_file("bulletins/05-art.ans", "\x1b[1;36m\xC9\xCD\xBB\x1b[0m art\r\n\x1a SAUCE junk", plat_fake_now + 30);
	type(&x, "b");
	for (int i = 0; i < 12 && !on_screen(&x, " art"); i++) type(&x, "\r");
	CK(on_screen(&x, "\xE2\x95\x94\xE2\x95\x90\xE2\x95\x97 art") && !on_screen(&x, "SAUCE"),
		".ans: CP437 box characters shown as themselves, SAUCE left out");
	type(&x, "q");

	// -- 14. every node busy --
	hang(&x);
	{
		static caller_t y;
		call(&x, T_VT, "telnet", "");
		call(&y, T_VT, "telnet", "");           // with a: all three nodes
		bbs_conn_t w = { "telnet", "198.51.100.1", "" };
		CK(bbs_connect(&w) < 0, "a caller over the limit is refused");
		CK(strstr(bbs_busy_text(), "busy") != NULL, "... with a message");
		hang(&y);
		hang(&x);
	}

	// -- 15. timeouts --
	call(&x, T_VT, "telnet", "");
	tick(130000);
	pump(&x);
	CK(x.closed && on_screen(&x, "Too slow"), "too slow to log in");
	call(&x, T_VT, "telnet", "");
	login(&x, "J\xC3\xBCrgen", "newpass9");
	tick(10 * 60000);
	pump(&x);
	CK(!x.closed, "ten minutes idle is fine");
	tick(6 * 60000);
	pump(&x);
	CK(x.closed && on_screen(&x, "Nothing typed"), "fifteen is not");

	// -- 16. a lone ESC --
	pump(&a);
	CK(a.closed && on_screen(&a, "Nothing typed"), "(the sysop's call, idle all this while, has ended too)");
	call(&a, T_VT, "local", "");
	login(&a, "Phil", "hunter22");
	CK(bbs_node[a.node]->logged_in && on_screen(&a, "Main menu"), "(the sysop again, locally)");
	type(&a, "p");
	bbs_input(a.node, (const uint8_t *)"\x1b", 1);
	tick(400);
	pump(&a);
	CK(bbs_node[a.node]->state == N_MENU, "a lone ESC leaves the profile");
	bbs_input(a.node, (const uint8_t *)"\x1b[A", 3);
	pump(&a);
	CK(bbs_node[a.node]->state == N_MENU, "an arrow key is not ESC");

	// -- 17. pipe codes and colour --
	{
		node_t *n = bbs_node[a.node];
		uint32_t before = n->out_len;
		out_mci(n, "|12red|| |XY|1");
		const uint8_t *p;
		uint32_t len = bbs_output(a.node, &p);
		char got[64] = { 0 };
		memcpy(got, p + before, len - before < 63 ? len - before : 63);
		CK(strstr(got, "\x1b[1;31mred|") && strstr(got, "|XY|1"),
			"|12 is bright red, || is |, unknown codes are text (%s)", got + 1);
		pump(&a);
	}

	// ================================================================
	// messages (phase 4)
	// ================================================================
	type(&a, "\r");                       // (back to the menu)
	CK(on_screen(&a, "[N] New messages") && on_screen(&a, "[F] Forums") && on_screen(&a, "[M] Mail"),
		"the menu has messages now");
	{
		char p[300], idbuf[40] = "";
		snprintf(p, sizeof(p), "%s/node.id", dir);
		FILE *f = fopen(p, "r");
		CK(f && fgets(idbuf, sizeof(idbuf), f) && strlen(idbuf) == 17 && !strncmp(idbuf, bbs_node_id, 16) &&
			strlen(bbs_node_id) == 16,
			"node.id: 16 hex digits, made once");
		if (f) fclose(f);
		snprintf(p, sizeof(p), "%s/forums.cfg", dir);
		CK(access(p, R_OK) == 0 && bbs_nareas == 3 && !strcmp(bbs_area[1].tag, "general"),
			"a first forums.cfg: general and zeitlos, after the mail");
	}

	// -- 18. posting --
	type(&a, "f");
	CK(on_screen(&a, "Forums") && on_screen(&a, "General") && on_screen(&a, "Anything at all"), "the forum list");
	type(&a, "1\r");
	CK(bbs_node[a.node]->state == N_AREA_MENU && on_screen(&a, "0 messages"), "into General");
	type(&a, "w");
	type(&a, "Hello world\r");
	CK(bbs_node[a.node]->state == N_EDIT && on_screen(&a, "/s") && on_screen(&a, "  1>"), "the editor, with its help");
	type(&a, "A line to delete.\r");
	type(&a, "The first real line.\r");
	// 90 characters of words: wraps at the last space before 76
	type(&a, "alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu nu xi omicron pi rho sigma\r");
	{
		node_t *n = bbs_node[a.node];
		const char *l3 = strstr(n->ed, "alpha");
		const char *nl = l3 ? strchr(l3, '\n') : NULL;
		CK(n->ed_lines == 4 && nl && nl - l3 <= 76 && nl[-1] != ' ' && strstr(n->ed, "\nsigma\n") == NULL &&
			strstr(nl + 1, "sigma") != NULL, "a long line wraps at a word, the word moves down (%d lines)", n->ed_lines);
	}
	type(&a, "/d 1\r");
	CK(bbs_node[a.node]->ed_lines == 3 && on_screen(&a, "  1> The first real line."), "/d deletes a line, and lists");
	type(&a, "/s\r");
	CK(on_screen(&a, "Saved, #1.") && msg_count(1) == 1, "saved as #1");
	{
		msg_t m; idx_t e; char body[256], want[80];
		CK(msg_head(1, 1, &m, &e) && !strcmp(m.from, "Phil") && m.from_id == 1 && !strcmp(m.subject, "Hello world"),
			"its headers read back");
		snprintf(want, sizeof(want), "%s:general:1", bbs_node_id);
		CK(!strcmp(m.id, want), "its id: node, area, number (%s)", m.id);
		msg_body(1, &m, body, sizeof(body));
		CK(!strncmp(body, "The first real line.\nalpha", 26), "its body");
		char p[300]; snprintf(p, sizeof(p), "%s/msgs/general.idx", dir);
		struct stat st; CK(stat(p, &st) == 0 && st.st_size == 32, "one 32-byte index entry");
	}
	type(&a, "x");
	CK(bbs_node[a.node]->state == N_AREA_MENU, "back to the forum's menu");
	type(&a, "q");                           // to the forum list
	type(&a, "\r");                          // to the menu
	CK(bbs_node[a.node]->state == N_MENU, "(Phil at the menu)");

	// -- 19. another caller: the new-scan, a reply with a quote --
	call(&x, T_VT, "telnet", "");
	login(&x, "J\xC3\xBCrgen", "newpass9");
	CK(bbs_node[x.node]->state == N_MENU, "(J\xC3\xBCrgen at the menu)");
	type(&x, "n");
	CK(on_screen(&x, "Hello world") && on_screen(&x, "From     Phil") && on_screen(&x, "The first real line.") &&
		on_screen(&x, "[N]ext"), "the new-scan finds Phil's message");
	type(&x, "r");
	CK(on_screen(&x, "Subject: Re: Hello world"), "a reply: Re: and the subject");
	type(&x, "\r");
	CK(on_screen(&x, "Quote the message?"), "asked about quoting");
	type(&x, "y");
	CK(on_screen(&x, "Phil wrote:") && on_screen(&x, "> The first real line."), "quoted");
	type(&x, "Agreed.\r/s\r");
	CK(on_screen(&x, "Saved, #2."), "the reply saved");
	{
		msg_t m, first;
		msg_head(1, 2, &m, NULL);
		msg_head(1, 1, &first, NULL);
		CK(m.reply == 1 && !strcmp(m.reply_id, first.id) && !strcmp(m.subject, "Re: Hello world"),
			"a reply knows what it answers, by number here and by id everywhere");
	}
	type(&x, "x");                            // back to the message's prompt
	type(&x, "n");
	CK(on_screen(&x, "Agreed."), "Next: the following message -- here, the reply just written");
	type(&x, "n");
	CK(on_screen(&x, "That is everything new"), "and the scan ends");
	type(&x, "x");
	type(&x, "f");
	CK(on_screen(&x, "General") && on_screen(&x, "   0    2"), "no news in General: 0 new of 2");

	// -- 20. mail, and who sees it --
	type(&x, "\r");
	type(&x, "m");
	type(&x, "w");
	type(&x, "nobody\r");
	CK(on_screen(&x, "No such user"), "mail to nobody");
	type(&x, "phil\r");
	type(&x, "Private\r");
	type(&x, "Just for you. |CL\x07\r/s\r");
	CK(on_screen(&x, "Saved, #1.") && msg_count(0) == 1, "a letter");
	type(&x, "x");
	type(&x, "r");
	CK(on_screen(&x, "Nothing new here"), "J\xC3\xBCrgen does not see a letter to Phil");
	type(&x, "q");
	type(&a, "\r");
	CK(on_screen(&a, "You have 1 new letter."), "Phil is told at the menu");
	type(&a, "n");
	CK(on_screen(&a, "Private") && on_screen(&a, "To       Phil"), "the new-scan: mail first");
	CK(on_screen(&a, "Just for you. |CL"), "a | in a message is text; the BEL is gone");
	type(&a, "n");
	CK(on_screen(&a, "Re: Hello world") && on_screen(&a, "Agreed."), "then the forum: Jürgen's reply");
	type(&a, "n");
	CK(on_screen(&a, "That is everything new"), "then nothing");
	type(&a, "x");
	CK(!on_screen(&a, "new letter"), "a read letter is not new");
	{ idx_t e; CK(msg_idx(0, 1, &e) && (e.flags & IDX_READ), "read: a flag on the letter"); }

	// -- 21. deleting --
	type(&x, "f");
	type(&x, "1\r");
	type(&x, "a");
	CK(on_screen(&x, "Hello world"), "from the start");
	type(&x, "d");
	CK(on_screen(&x, "Only its writer"), "not someone else's");
	type(&x, "n");
	type(&x, "d");
	CK(on_screen(&x, "Deleted."), "your own, yes");
	{ idx_t e; CK(msg_idx(1, 2, &e) && (e.flags & IDX_DELETED), "marked, not removed"); }
	type(&x, "x");
	type(&x, "a");
	type(&x, "n");
	CK(on_screen(&x, "No more messages"), "and not shown again");
	type(&x, "x");
	type(&x, "q");
	type(&x, "\r");

	// -- 22. levels --
	write_file("forums.cfg", "general; General; 0; 10; Anything\nannounce; News; 0; 100; From the sysop\n"
		"secret; Secret; 200; 200; Sysops\nbad tag!; Oops\nmail; Mine\n", 0);
	areas_load();
	CK(bbs_nareas == 4 && area_find("secret") == 3 && area_find("bad tag!") < 0 && msg_count(1) == 2,
		"forums.cfg: good lines kept, bad ones and 'mail' refused; counts kept");
	type(&x, "f");
	CK(on_screen(&x, "News") && !on_screen(&x, "Secret"), "a forum above your level is not listed");
	type(&x, "2\r");
	CK(bbs_node[x.node]->area == 2 && !on_screen(&x, "[W]"), "nor can you write where the level says not");
	type(&x, "q");
	type(&x, "\r");

	// -- 23. who is writing; hanging up mid-message --
	type(&x, "f");
	type(&x, "1\r");
	type(&x, "w");
	type(&x, "Unfinished\r");
	type(&x, "half a thought");
	type(&a, "w");
	CK(on_screen(&a, "writing a message"), "who is online: writing");
	type(&a, "x");
	hang(&x);
	CK(bbs_node[x.node]->ed == NULL, "hanging up frees the editor (and ASan checks nothing leaks)");
	CK(msg_count(1) == 2, "an unfinished message is not saved");
	{
		char p[300], t[64] = "";
		snprintf(p, sizeof(p), "%s/lastread/2.txt", dir);
		FILE *f = fopen(p, "r");
		CK(f && fgets(t, sizeof(t), f) && !strcmp(t, "general 2\n"), "last read, kept per user (%s)", t);
		if (f) fclose(f);
	}

	// -- 23b. a message that fills the editor --
	call(&x, T_VT, "telnet", "");
	login(&x, "J\xC3\xBCrgen", "newpass9");
	type(&x, "f"); type(&x, "1\r"); type(&x, "w"); type(&x, "Long\r");
	for (int i = 0; i < 130; i++)
		type(&x, "0123456789 0123456789 0123456789 0123456789 0123456789 0123456789\r");
	CK(on_screen(&x, "The message is full") && bbs_node[x.node]->ed_len < MSG_BODY_MAX,
		"the editor fills up and says so, never past its buffer (%u of %d)",
		(unsigned)bbs_node[x.node]->ed_len, MSG_BODY_MAX);
	type(&x, "/s\r");
	CK(on_screen(&x, "Saved"), "and what fits is saved");
	hang(&x);
	int before_crash = msg_count(1);
	CK(before_crash == 3, "(three in General now)");

	// -- 24. a crash between the log and the index --
	{
		char p[300];
		msg_t m;
		memset(&m, 0, sizeof(m));
		strcpy(m.from, "Phil"); m.from_id = 1; strcpy(m.subject, "Third");
		CK(msg_post(1, &m, "three\n", 6) == 4, "a fourth message");
		snprintf(p, sizeof(p), "%s/msgs/general.idx", dir);
		CK(truncate(p, 96) == 0, "(its index entry lost, as if the power went)");
		areas_load();
		CK(msg_count(1) == 4 && msg_head(1, 4, &m, NULL) && !strcmp(m.subject, "Third"),
			"at start: indexed again from the log");
		// a torn write: half a record at the end of the log
		snprintf(p, sizeof(p), "%s/msgs/general.log", dir);
		FILE *f = fopen(p, "a"); fputs("ZM1 500\nid: torn", f); fclose(f);
		areas_load();
		CK(msg_count(1) == 4, "a torn record is not a message");
		memset(&m, 0, sizeof(m));
		strcpy(m.from, "Phil"); m.from_id = 1; strcpy(m.subject, "Fourth");
		CK(msg_post(1, &m, "four", 4) == 5 && msg_head(1, 5, &m, NULL) && !strcmp(m.subject, "Fourth"),
			"the next message goes where the torn one was");
		char body[32];
		msg_body(1, &m, body, sizeof(body));
		CK(!strcmp(body, "four\n"), "a body gets its final newline");
		{
			static char logtext[16384];
			idx_t last;
			FILE *lf = fopen(p, "rb");
			size_t ln = lf ? fread(logtext, 1, sizeof(logtext) - 1, lf) : 0;
			if (lf) fclose(lf);
			logtext[ln] = 0;
			CK(!strstr(logtext, "id: torn") && msg_idx(1, 5, &last) && last.off + last.len == ln,
				"... over it: nothing of the torn record is left in the log, which ends where the index says");
		}
		areas_load();
		CK(msg_count(1) == 5, "and nothing is left to repair");
		// torn, but with a length that fits: 7 + 40 bytes, not ending in \n
		f = fopen(p, "a");
		fputs("ZM1 40\nid: t\nsubject: torn\n\nbody that never ended", f);
		fclose(f);
		areas_load();
		CK(msg_count(1) == 5, "a torn record whose length fits is still not a message");
		memset(&m, 0, sizeof(m));
		strcpy(m.from, "Phil"); m.from_id = 1; strcpy(m.subject, "Hi\nfrom: Mallory\nfrom_id: 9");
		int k = msg_post(1, &m, "x", 1);
		CK(k == 6 && msg_head(1, 6, &m, NULL) && !strcmp(m.from, "Phil") && m.from_id == 1,
			"a newline in a header value cannot forge another header");
	}

	hang(&a);
	// -- 25. a restart --
	CK(bbs_init(dir) && msg_count(0) == 1 && msg_count(1) == 6, "a restart: everything where it was");

	// -- 26. writing full-screen: zetta (docs/zetta.md) --
	{
		caller_t z;
		call(&z, T_VT, "telnet", "");
		join(&z, "Zoe", "hunter22", "Here");			// join() chose the line editor ...
		type(&z, "p"); type(&z, "e");					// ... so back to full-screen, as a caller would
		CK(!(bbs_node[z.node]->user.flags & USER_F_LINE_EDITOR) && on_screen(&z, "Editor     full-screen"),
			"the profile: full-screen again, and says so");
		type(&z, "q");
		int before = msg_count(1);

		// a forum post
		type(&z, "f"); type(&z, "1\r"); type(&z, "w");
		CK(bbs_node[z.node]->state == N_ZETTA && on_screen(&z, "Subject:") && on_screen(&z, "^S Post") &&
			on_screen(&z, "Esc Menu"), "W: the full-screen editor, a Subject field and the keys");
		type(&z, "Full screen\r");
		const char *para = "This paragraph is typed without a single Enter in it, and it is long enough that "
			"the editor has to wrap it more than once on an eighty column screen, words kept whole.";
		type(&z, para);
		CK(on_screen(&z, "\nThis paragraph is typed") && on_screen(&z, "\nthat the editor has to wrap") &&
			on_screen(&z, "\nkept whole."), "the text on the screen, wrapped at words: each row starts with a whole one");
		type(&z, "\x13");								// Ctrl-S
		CK(bbs_node[z.node]->state != N_ZETTA && msg_count(1) == before + 1 && on_screen(&z, "Saved"),
			"Ctrl-S: posted, and the editor gone");
		{
			msg_t m; idx_t e; static char body[4096];
			CK(msg_head(1, before + 1, &m, &e) && !strcmp(m.subject, "Full screen") && !strcmp(m.from, "Zoe"), "its headers");
			uint32_t bl = msg_body(1, &m, body, sizeof(body));
			body[bl] = 0;
			bool narrow = true;
			for (char *l = body; *l; ) { char *nl = strchr(l, '\n'); int ll = nl ? (int)(nl - l) : (int)strlen(l); if (ll > 79) narrow = false; if (!nl) break; l = nl + 1; }
			for (char *q = body; *q; q++) if (*q == '\n') *q = ' ';
			while (bl && (body[bl - 1] == ' ')) body[--bl] = 0;
			CK(narrow && !strcmp(body, para), "stored as lines within 79 columns, every word in its place");
		}
		type(&z, "\r"); type(&z, "q"); type(&z, "\r");		// the forum's menu, the list, the main menu

		// a letter: To checked against the users
		int mails = msg_count(0);
		type(&z, "m"); type(&z, "w");
		CK(bbs_node[z.node]->state == N_ZETTA && on_screen(&z, "To:"), "a letter: a To field");
		type(&z, "nobody\r");
		CK(on_screen(&z, "No such user."), "To: nobody -- no such user");
		type(&z, "\x7f\x7f\x7f\x7f\x7f\x7fphil\rHello Phil\rA letter from Zoe.");
		type(&z, "\x13");
		{
			msg_t m; idx_t e;
			CK(msg_count(0) == mails + 1 && msg_head(0, mails + 1, &m, &e) && !strcmp(m.to, "Phil") && m.to_id == 1,
				"sent -- to Phil, the account's own spelling");
		}
		type(&z, "\r"); type(&z, "q");

		// no subject: said at once
		type(&z, "f"); type(&z, "1\r"); type(&z, "w"); type(&z, "\rNo subject here.");
		type(&z, "\x13");
		CK(bbs_node[z.node]->state == N_ZETTA && on_screen(&z, "A subject, please."),
			"posting with no subject: refused, and said at once");
		type(&z, "\x18"); type(&z, "y"); type(&z, "\r"); type(&z, "q"); type(&z, "\r");

		// discarding
		type(&z, "f"); type(&z, "1\r"); type(&z, "w"); type(&z, "Nope\rNot to be posted.");
		type(&z, "\x18");
		CK(on_screen(&z, "Discard this message? (y/n)"), "Ctrl-X with something written: it asks");
		type(&z, "n");
		CK(bbs_node[z.node]->state == N_ZETTA, "n: still writing");
		type(&z, "\x18"); type(&z, "y");
		CK(bbs_node[z.node]->state != N_ZETTA && msg_count(1) == before + 1 && on_screen(&z, "Not written."),
			"y: gone, nothing posted");
		type(&z, "\r");

		// a reply, with the quote window
		type(&z, "a");									// from the first
		type(&z, "r");
		CK(bbs_node[z.node]->state == N_ZETTA && on_screen(&z, "Re: ") && on_screen(&z, "^Q Quote"),
			"R: a reply, its subject Re:, and Quote offered");
		type(&z, "\x11");								// Ctrl-Q
		CK(on_screen(&z, "[ ] "), "Ctrl-Q: the original's lines to choose from");
		type(&z, " \r");
		type(&z, "My reply.");
		type(&z, "\x13");
		{
			msg_t m; idx_t e; static char body[4096];
			CK(msg_count(1) == before + 2 && msg_head(1, before + 2, &m, &e) && m.reply > 0, "the reply posted, as a reply");
			uint32_t bl = msg_body(1, &m, body, sizeof(body));
			body[bl] = 0;
			CK(!strncmp(body, "> ", 2) || !strncmp(body, ">", 1), "the chosen line quoted: '> ' before it (%.30s)", body);
			CK(strstr(body, "My reply.") != NULL, "and the answer after it");
		}
		type(&z, "\r");

		// hanging up in the middle of a message: nothing left behind. The
		// heap, measured -- the node is cleared on hang-up, so its pointers
		// read NULL whether the memory was freed or not.
		// back from the reader (after the reply) to the forum's menu first
		for (int i = 0; i < 4 && bbs_node[z.node]->state != N_AREA_MENU; i++) type(&z, "q");
		size_t heap_before = heap_in_use();
		type(&z, "w"); type(&z, "Half\rhalf a mess");
		CK(bbs_node[z.node]->state == N_ZETTA && heap_in_use() > heap_before + 4000,
			"(writing: the editor open, its memory in use)");
		int zn = z.node;
		hang(&z);
		CK(bbs_node[zn]->state == N_FREE && heap_in_use() == heap_before,
			"hung up while writing: every byte of the editor freed (%ld left)", (long)(heap_in_use() - heap_before));
	}

	CK(nul_bytes == 0, "not one NUL byte sent to anyone (%ld)", nul_bytes);
	printf("bbs test: %d checks, %d failed\n", checks, fails);
	char cmd[300];
	snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
	if (!fails) { int r = system(cmd); (void)r; }
	return fails != 0;
}
