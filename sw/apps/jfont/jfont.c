/*
 * jfont -- the Japanese font, held once for every app
 *
 *   > run jfont
 *
 * or `system.font.japanese: yes` in /zeitlos.cfg, and wm starts it at
 * boot. Loads /font/jp12.zfn (12x12 Shinonome, public domain; built by
 * tools/gen_jfont.py) into its own memory, writes the descriptor in
 * sw/common/zjfont.h in front of it, and waits. Apps draw from it
 * directly: every byte has one physical address any process may read.
 * It answers no messages -- see zjfont.h for why finding it takes none.
 *
 * Costs about 190KB of RAM while it runs, which is why it only runs when
 * asked: on a 1MB board that is a fifth of memory, for a script most
 * people never see. See docs/text_encoding.md, "Japanese".
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zfsapp.h"
#include "../../common/zjfont.h"

// Room for the font and a little over: jp12.zfn is 178,868 bytes.
#define JF_MAX (184 * 1024)

static struct {
	z_jfont_desc_t	d;
	uint8_t			font[JF_MAX];
} jf __attribute__((aligned(4)));

// -- console output, without stdio --
//
// printf would pull ~40KB of newlib into a process whose whole point is
// to be as small as the font it holds. uart_putc() is in zeitlos.c.
void uart_putc(char c);

static void say(const char *s) {
	while (*s) { if (*s == '\n') uart_putc('\r'); uart_putc(*s++); }
}

static void say_hex(uint32_t v) {
	char b[11] = "0x00000000";
	for (int i = 0; i < 8; i++) b[9 - i] = "0123456789abcdef"[(v >> (4 * i)) & 15];
	say(b);
}

static void say_dec(uint32_t v) {
	char b[11];
	int i = 10;
	b[i] = 0;
	do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v && i);
	say(&b[i]);
}

// Physical address of something in this process -- see blit_phys_addr()
// in zgfx.c, the same translation.
static uint32_t phys(const void *p) {
	uint32_t v = (uint32_t)(uintptr_t)p;
	uint32_t base = reg_mtu_base;
	if (base == 0 || (v & 0xF0000000u) != 0x80000000u) return v;
	return base + (v & 0x0FFFFFFFu);
}

int main(void) {

	char name[24];

	// One is enough: a second would be 190KB for nothing. The registry
	// numbers names in order, so anything but "jfont0" means one is
	// already running.
	if (!z_pid_register(Z_JFONT_PROC, name, sizeof(name)) || strcmp(name, Z_JFONT_NAME)) {
		say("jfont: already running\n");
		return 0;
	}

	int size = fs_size((char *)Z_JFONT_PATH);
	if (size < 12 || size > JF_MAX) {
		say("jfont: " Z_JFONT_PATH " missing or too large\n");
		return 1;
	}

	int h = fs_open_read(Z_JFONT_PATH);
	if (h < 0) {
		say("jfont: cannot open " Z_JFONT_PATH "\n");
		return 1;
	}

	int n = 0;
	while (n < size) {
		int r = fs_read_chunk(h, jf.font + n, size - n);
		if (r <= 0) break;
		n += r;
	}
	fs_close_handle(h);

	if (n != size || memcmp(jf.font, "ZFN1", 4)) {
		say("jfont: " Z_JFONT_PATH " is not a font file\n");
		return 1;
	}

	// The descriptor, magic last: until magic0 is written, no client can
	// match it, so no client ever sees a half-loaded font.
	uint32_t at = phys(&jf.d);
	jf.d.len = (uint32_t)n;
	jf.d.check = z_jfont_check(at);
	jf.d.magic1 = Z_JFONT_MAGIC1;
	jf.d.magic0 = Z_JFONT_MAGIC0;

	uint32_t count = (uint32_t)jf.font[8] | ((uint32_t)jf.font[9] << 8) |
		((uint32_t)jf.font[10] << 16) | ((uint32_t)jf.font[11] << 24);
	say("jfont: ");
	say_dec(count);
	say(" glyphs at ");
	say_hex(at);
	say("\n");

	// Nothing to do but exist. Messages are read and dropped so a stray
	// one cannot fill the mailbox.
	for (;;) {
		z_msg_t msg;
		while (z_msg_read(&msg) == Z_OK) {}
		z_proc_wait(0);
	}

}
