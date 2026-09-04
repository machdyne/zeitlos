/*
 * Render sw/apps/mmod's panel to an image.
 *
 *   cc -std=gnu99 -Wall -I sw/common -o /tmp/render \
 *      sw/apps/mmod/tests/render.c \
 *      sw/common/zwin.c sw/common/zwidget.c sw/common/zfont_data.c \
 *      sw/common/zobj.c sw/common/zeitlos.c
 *   /tmp/render /tmp/panel.pbm
 *
 * See sw/common/tests/zrender.h for what this does and what it cannot
 * catch. In short: the panel, zwin.c and zwidget.c are the REAL
 * sources; only the pixel plotting is software.
 */

#include "../../../common/tests/zrender.h"

#include "../panel.c"

// -- stubs -------------------------------------------------------
//
// Only what panel.c's drawing path reaches. Do-nothing on purpose:
// this renders geometry, and anything needing a pin or a message is
// not geometry.

bool z_gpio_present(void) { return true; }
uint32_t z_gpio_port_count(void) { return 1; }
uint8_t z_gpio_in_get(uint32_t p) { (void)p; return 0xff; }
uint8_t z_gpio_dir_get(uint32_t p) { (void)p; return 0; }
uint8_t z_gpio_out_get(uint32_t p) { (void)p; return 0; }
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

// zmmod.c is linked (see the build line above) so the panel's device
// fields are populated by the real profile logic; only the SPI layer
// under it is stubbed, which is where the pins would be.
bool z_spi_init(z_spi_t *s) { (void)s; return true; }
void z_spi_select(z_spi_t *s, bool on) { (void)s; (void)on; }
uint8_t z_spi_xfer8(z_spi_t *s, uint8_t o) { (void)s; (void)o; return 0xff; }
void z_spi_xfer(z_spi_t *s, const uint8_t *t, uint8_t *r, uint32_t n) {
	(void)s; (void)t; if (r) memset(r, 0xff, n); }

int main(int argc, char **argv) {

	const char *out = argc > 1 ? argv[1] : "/tmp/panel.pbm";

	if (!z_render_open(&win, WIN_W, WIN_H)) {
		printf("render: skipped (cannot map the VRAM address)\n");
		return 77;
	}

	state_init();
	widgets_init();

	// A fully populated device with the LONGEST text each field will
	// ever hold. An idle panel of dashes hides exactly the overflow
	// this exists to find -- what has to fit is the worst case.
	dev_detected = true;
	ss_ok = true;
	dev_id[0] = 0xef; dev_id[1] = 0x40; dev_id[2] = 0x19;
	dev_class = C_NOR;
	dev_size = 32u * 1024u * 1024u;
	dev_page = 256;
	dev_erase = 4096;
	addr_idx = 2;
	range_start = 0;
	range_len = 0x02000000;
	snprintf(file_name, sizeof(file_name), "firmware-2026-09.bin");
	snprintf(file_path, sizeof(file_path), "/sd/firmware-2026-09.bin");
	file_size = 33554432;
	progress = 47;
	snprintf(status, sizeof(status),
		"READ 15.4 MB / 32.0 MB   62 KB/s   ETA 4m28s");
	snprintf(detail, sizeof(detail),
		"sector 0x01f0f000 of 0x02000000   cancel is safe between sectors");
	snprintf(lbl_backend, sizeof(lbl_backend), "bit-bang");
	snprintf(lbl_rate, sizeof(lbl_rate), "62 KB/s");

	relabel();
	layout();
	repaint();

	z_render_write(out, &win, 3);

	return 0;

}
