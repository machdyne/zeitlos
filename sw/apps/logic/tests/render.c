/*
 * Render sw/apps/logic's panel to an image.
 *
 *   cc -std=gnu99 -Wall -I sw/common -o /tmp/render \
 *      sw/apps/logic/tests/render.c \
 *      sw/common/zwin.c sw/common/zwidget.c sw/common/zfont_data.c \
 *      sw/common/zobj.c sw/common/zeitlos.c
 *   /tmp/render /tmp/logic.pbm
 *
 * See sw/common/tests/zrender.h. logic.c, zwin.c and zwidget.c are the
 * REAL sources; only the pixel plotting is software.
 *
 * This panel is the reason the renderer exists -- it shipped wrong
 * three times against passing arithmetic tests. Look at the output
 * before changing its layout.
 */

#include "../../../common/tests/zrender.h"

#define main logic_main_unused
#include "../logic.c"
#undef main

// -- stubs -------------------------------------------------------

bool z_gpio_present(void) { return true; }
uint32_t z_gpio_port_count(void) { return 1; }
uint8_t z_gpio_in_get(uint32_t p) { (void)p; return 0xa5; }
uint8_t z_gpio_dir_get(uint32_t p) { (void)p; return 0x0f; }
uint8_t z_gpio_out_get(uint32_t p) { (void)p; return 0x05; }
void z_gpio_dir_set(uint32_t p, uint8_t m) { (void)p; (void)m; }
void z_gpio_out_put(uint32_t p, uint8_t v) { (void)p; (void)v; }
void z_gpio_mode(uint32_t p, uint32_t n, z_gpio_mode_t m) {
	(void)p; (void)n; (void)m; }
z_gpio_mode_t z_gpio_mode_get(uint32_t p, uint32_t n) {
	(void)p; (void)n; return Z_GPIO_IN; }
bool z_gpio_read(uint32_t p, uint32_t n) { (void)p; (void)n; return false; }
void z_gpio_write(uint32_t p, uint32_t n, bool v) {
	(void)p; (void)n; (void)v; }
void z_gpio_toggle(uint32_t p, uint32_t n) { (void)p; (void)n; }
void z_gpio_od_write(uint32_t p, uint32_t n, bool v) {
	(void)p; (void)n; (void)v; }
void z_led_set(bool b) { (void)b; }
void z_led_bar_set(uint8_t b) { (void)b; }

// Everything else logic.c calls comes from zwin.c and zeitlos.c,
// which are linked. Only the GPIO layer is stubbed, because that one
// would touch pins.

int main(int argc, char **argv) {

	const char *out = argc > 1 ? argv[1] : "/tmp/logic.pbm";
	uint32_t i;

	if (!z_render_open(&win, WIN_W, WIN_H)) {
		printf("render: skipped (cannot map the VRAM address)\n");
		return 77;
	}

	nports = 1;
	port = 0;

	// A mixture of modes, and a capture in the buffer -- an idle panel
	// with no trace hides everything the waveform area can get wrong.
	for (i = 0; i < 8; i++) {
		mode[i] = (chmode_t)(i % 3);
		drive_hi[i] = (i & 1) != 0;
	}
	in_live = 0xa5;

	sample_count = 1024;
	sample_cycles = 1024u * 480u;
	for (i = 0; i < sample_count; i++)
		samples[i] = (uint8_t)((i >> 1) ^ (i >> 4) ^ (i >> 7));

	trig_ch = 0;
	trig_edge = T_RISE;
	rate_idx = 4;
	depth_idx = 3;

	snprintf(status, sizeof(status),
		"1024 Sa @ 98 kSa/s  ch0: 84 edges, 4021 Hz, 49%% hi");
	snprintf(decode, sizeof(decode), "I2C: S 3cw+ 01+ 5a- P");

	widgets_init();
	relabel();
	layout();
	repaint();

	z_render_write(out, &win, 2);

	return 0;

}
