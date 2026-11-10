/*
 * The devices' registers, one behaviour at a time (docs/zsys.md,
 * "Testing"). Each expectation is worked out by hand from the RTL --
 * rtl/gpu/gpu_blit.v, gpu_raster.v, rtl/spim.v with an SD card behind
 * it, rtl/rtc.v, rtl/socctl.v -- so a change to zdev.c that drifts
 * from the hardware shows here, not as a wrong pixel in a screenshot.
 *
 *   make test-zdev
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "zsys.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

static zsys_t s;

#define BLIT 0xd0000000u
#define RAST 0xa0000000u
#define SPI  0xb0000000u
#define SOC  0x70000200u
#define RTC  0x70000300u

static void wr(uint32_t a, uint32_t v) { zdev_write(&s, a, v, 15); }
static uint32_t rd(uint32_t a) { bool ok; return zdev_read(&s, a, &ok); }
static uint32_t vw(uint32_t row, uint32_t word) { uint32_t v; memcpy(&v, s.vram + row * 80 + word * 4, 4); return v; }
static void vset(uint32_t row, uint32_t word, uint32_t v) { memcpy(s.vram + row * 80 + word * 4, &v, 4); }
static void clear(void) { memset(s.vram, 0, s.vram_size); }

static void rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
	wr(BLIT + 0x08, x); wr(BLIT + 0x0c, y); wr(BLIT + 0x10, w); wr(BLIT + 0x14, h);
}

// -- the blitter --

static void test_fill(void) {
	clear();
	rect(0, 0, 64, 2);
	wr(BLIT + 0x18, 0xaaaaaaaau);
	wr(BLIT, 1 | 2);					// START | FILL, unclipped
	CHECK(vw(0, 0) == 0xaaaaaaaau && vw(0, 1) == 0xaaaaaaaau && vw(1, 1) == 0xaaaaaaaau && vw(0, 2) == 0,
		"unclipped fill: %08x %08x %08x", vw(0, 0), vw(1, 1), vw(0, 2));

	clear();
	rect(4, 10, 40, 1);					// x 4..43
	wr(BLIT + 0x18, ~0u);
	wr(BLIT, 1 | 2 | 4);
	CHECK(vw(10, 0) == 0xfffffff0u && vw(10, 1) == 0x00000fffu && vw(9, 0) == 0 && vw(11, 0) == 0,
		"clipped fill, edge masks: %08x %08x", vw(10, 0), vw(10, 1));
	uint32_t rows[ZSYS_ROWS / 32];
	zsys_dirty_take(&s, rows);
	CHECK(rows[0] & (1u << 10), "the filled row is dirty");
	zsys_dirty_take(&s, rows);
	CHECK(!(rows[0] & (1u << 10)), "and clean once taken");

	clear();
	wr(BLIT + 0x50, 8);					// the scissor: x from 8
	rect(0, 0, 16, 1);
	wr(BLIT, 1 | 2 | 4);
	CHECK(vw(0, 0) == 0x0000ff00u, "scissor: %08x", vw(0, 0));
	wr(BLIT + 0x50, 0);

	clear();
	rect(0, 0, 0, 5);
	wr(BLIT, 1 | 2 | 4);					// clipped to nothing: nothing
	CHECK(vw(0, 0) == 0, "an empty clipped fill writes nothing");
	rect(0, 3, 32, 0);
	wr(BLIT, 1 | 2);					// unclipped, height 0: one row, as ST_NEXT tests after
	CHECK(vw(3, 0) == ~0u && vw(4, 0) == 0, "unclipped height 0 still writes a row");

	clear();
	rect(0, 0, 32, 1);
	wr(BLIT + 0x18, 0x0f0f0f0fu);
	wr(BLIT, 1 | 2 | 4 | (2 << 5));			// ROP XOR
	wr(BLIT, 1 | 2 | 4 | (2 << 5));
	CHECK(vw(0, 0) == 0, "XOR twice is nothing: %08x", vw(0, 0));
	CHECK((rd(BLIT) & 0x60) == 0x40, "CTRL reads the ROP back");

	clear();
	rect(0, 0, 32, 2);
	wr(BLIT + 0x18, 8);					// grey level 8
	wr(BLIT, 1 | 2 | 4 | (1 << 8));
	CHECK(vw(0, 0) == 0x55555555u && vw(1, 0) == 0xaaaaaaaau, "dither 8: %08x %08x", vw(0, 0), vw(1, 0));

	CHECK(rd(BLIT + 4) == 0, "STATUS: never busy");
	wr(BLIT + 0x08, 0x12345);
	CHECK(rd(BLIT + 0x08) == 0xffff, "coordinates saturate at 16 bits");
}

static void test_glyph(void) {
	clear();
	memset(s.glyph, 0, sizeof(s.glyph));
	s.glyph[0] = 0x80;					// the leftmost pixel only
	for (int w = 0; w < 2; w++) vset(0, w, ~0u);
	rect(30, 0, 0, 0);
	wr(BLIT + 0x1c, 0); wr(BLIT + 0x20, 8); wr(BLIT + 0x24, 1);
	wr(BLIT + 0x28, 1); wr(BLIT + 0x2c, 0);
	wr(BLIT, 1 | 8);					// GLYPH, unclipped
	CHECK(vw(0, 0) == 0x7fffffffu && vw(0, 1) == 0xffffffc0u,
		"a glyph straddling two words: %08x %08x", vw(0, 0), vw(0, 1));

	clear();
	s.glyph[1] = 0xff;
	wr(BLIT + 0x58, 34);					// scissor: x below 34
	rect(30, 0, 0, 0);
	wr(BLIT + 0x1c, 1);
	wr(BLIT, 1 | 8 | 4);					// GLYPH | CLIP
	CHECK(vw(0, 0) == 0xc0000000u && vw(0, 1) == 0x3u, "a clipped glyph: %08x %08x", vw(0, 0), vw(0, 1));
	wr(BLIT + 0x58, 640);
}

static void test_copy(void) {
	uint32_t src[2] = { 0x12345678u, 0x9abcdef0u };
	memcpy(s.ram + 0x1000, src, 8);
	clear();
	rect(0, 0, 32, 1);
	wr(BLIT + 0x30, 0x40001000u); wr(BLIT + 0x34, 8); wr(BLIT + 0x38, 0);
	wr(BLIT, 1 | 4 | 16);					// copy from main memory
	CHECK(vw(0, 0) == 0x12345678u, "a memory copy: %08x", vw(0, 0));
	wr(BLIT + 0x38, 8);
	wr(BLIT, 1 | 4 | 16);
	CHECK(vw(0, 0) == 0xf0123456u, "shifted by 8: %08x", vw(0, 0));
	CHECK(rd(BLIT + 0x48) == 4, "the probe counts four source reads");

	clear();
	for (int r = 1; r <= 3; r++) for (int w = 0; w < 20; w++) vset(r, w, 0x01010101u * r + w);
	rect(0, 0, 640, 3);
	wr(BLIT + 0x30, 0x20000000u + 80); wr(BLIT + 0x34, 80); wr(BLIT + 0x38, 0);
	wr(BLIT, 1 | 4);					// VRAM to VRAM: up a row
	CHECK(vw(0, 0) == 0x01010101u && vw(1, 19) == 0x02020202u + 19 && vw(2, 5) == 0x03030303u + 5,
		"scrolling up a row: %08x %08x %08x", vw(0, 0), vw(1, 19), vw(2, 5));

	uint32_t mask[2] = { 0x0000ffffu, 0 }, data[2] = { 0xffffffffu, 0 };
	memcpy(s.ram + 0x2000, mask, 8);
	memcpy(s.ram + 0x3000, data, 8);
	clear();
	vset(0, 0, 0xf0f0f0f0u);
	rect(0, 0, 32, 1);
	wr(BLIT + 0x30, 0x40002000u); wr(BLIT + 0x3c, 0x40003000u); wr(BLIT + 0x38, 0);
	wr(BLIT, 1 | 4 | 16 | (1 << 7));			// cookie cut
	CHECK(vw(0, 0) == 0xf0f0ffffu, "a masked sprite: %08x", vw(0, 0));
}

// -- the rasterizer --

static void test_raster(void) {
	clear();
	wr(RAST + 0, 0); wr(RAST + 4, 0); wr(RAST + 8, 3); wr(RAST + 12, 0); wr(RAST + 16, 1);
	wr(RAST + 20, 1);
	CHECK(vw(0, 0) == 0xf && rd(RAST + 28) == 4, "a short horizontal line: %08x, %u pixels", vw(0, 0), rd(RAST + 28));
	clear();
	wr(RAST + 8, 2); wr(RAST + 12, 2);
	wr(RAST + 20, 1);
	CHECK(vw(0, 0) == 1 && vw(1, 0) == 2 && vw(2, 0) == 4, "a diagonal");
	wr(RAST + 16, 2);
	wr(RAST + 20, 1);					// XOR the same line away
	CHECK(vw(0, 0) == 0 && vw(1, 0) == 0 && vw(2, 0) == 0, "XORed away");
	clear();
	wr(RAST + 16, 1);
	wr(RAST + 0, 0); wr(RAST + 4, 0); wr(RAST + 8, 639); wr(RAST + 12, 479);
	wr(RAST + 20, 1);
	CHECK(rd(RAST + 28) == 640, "a diagonal across the screen: %u pixels", rd(RAST + 28));
	CHECK(rd(RAST + 24) == 0, "never busy");
}

// -- the SD card --

static uint8_t xchg(uint8_t b) { wr(SPI, b); return (uint8_t)rd(SPI); }

static uint8_t cmd(uint8_t c, uint32_t arg, uint8_t *extra, int n) {
	xchg(0xff);
	uint8_t p[6] = { (uint8_t)(0x40 | c), (uint8_t)(arg >> 24), (uint8_t)(arg >> 16), (uint8_t)(arg >> 8), (uint8_t)arg, 0x95 };
	for (int i = 0; i < 6; i++) xchg(p[i]);
	uint8_t r = 0xff;
	for (int i = 0; i < 10 && (r & 0x80); i++) r = xchg(0xff);
	for (int i = 0; i < n; i++) extra[i] = xchg(0xff);
	return r;
}

static void test_sd(void) {
	char path[] = "out/test_card_XXXXXX";
	int fd = mkstemp(path);
	CHECK(fd >= 0, "a temporary card image");
	if (fd < 0) return;
	static uint8_t img[1024 * 1024];
	for (int i = 0; i < 512; i++) img[5 * 512 + i] = (uint8_t)(i * 7);
	CHECK(write(fd, img, sizeof(img)) == (ssize_t)sizeof(img), "written");
	close(fd);
	CHECK(zsys_sd_insert(&s, path, false), "inserted");

	wr(SPI + 8, 1);						// CS asserted
	uint8_t e[16];
	CHECK(cmd(0, 0, NULL, 0) == 0x01, "CMD0: idle");
	CHECK(cmd(8, 0x1aa, e, 4) == 0x01 && e[2] == 0x01 && e[3] == 0xaa, "CMD8 echoes its pattern");
	CHECK(cmd(55, 0, NULL, 0) == 0x01 && cmd(41, 1u << 30, NULL, 0) == 0x00, "ACMD41: ready");
	CHECK(cmd(58, 0, e, 4) == 0x00 && (e[0] & 0x40), "CMD58: high capacity");

	CHECK(cmd(17, 5, NULL, 0) == 0x00, "CMD17 accepted");
	uint8_t t = 0xff;
	for (int i = 0; i < 10 && t == 0xff; i++) t = xchg(0xff);
	bool same = t == 0xfe;
	for (int i = 0; i < 512; i++) same &= xchg(0xff) == (uint8_t)(i * 7);
	CHECK(same, "block 5 reads back");
	xchg(0xff); xchg(0xff);

	CHECK(cmd(24, 6, NULL, 0) == 0x00, "CMD24 accepted");
	xchg(0xff);
	xchg(0xfe);
	for (int i = 0; i < 512; i++) xchg((uint8_t)(255 - i));
	xchg(0xff); xchg(0xff);
	CHECK((xchg(0xff) & 0x1f) == 0x05, "the block is accepted");
	int busy = 0;
	while (xchg(0xff) != 0xff && busy < 100) busy++;
	CHECK(busy > 0 && busy < 100, "busy, then ready");
	CHECK(s.sd.img[6 * 512 + 1] == 254, "and is in the image");
	FILE *f = fopen(path, "rb");
	uint8_t back[2] = { 0, 0 };
	if (f) { fseek(f, 6 * 512, SEEK_SET); if (fread(back, 1, 2, f) != 2) back[0] = 0; fclose(f); }
	CHECK(back[0] == 255 && back[1] == 254, "and in the file");

	CHECK(cmd(9, 0, NULL, 0) == 0x00, "CMD9 accepted");
	t = 0xff;
	for (int i = 0; i < 10 && t == 0xff; i++) t = xchg(0xff);
	for (int i = 0; i < 16; i++) e[i] = xchg(0xff);
	uint32_t cs = e[9] + ((uint32_t)e[8] << 8) + ((uint32_t)(e[7] & 63) << 16) + 1;
	CHECK(t == 0xfe && (e[0] >> 6) == 1 && (cs << 10) == 2048, "the CSD: %u sectors", cs << 10);

	CHECK(cmd(18, 5, NULL, 0) == 0x00, "CMD18 accepted");
	int blocks = 0;
	for (int b = 0; b < 2; b++) {
		t = 0xff;
		for (int i = 0; i < 10 && t == 0xff; i++) t = xchg(0xff);
		uint8_t first = xchg(0xff);
		for (int i = 1; i < 514; i++) xchg(0xff);
		blocks += t == 0xfe && first == (b == 0 ? 0 : 255);
	}
	CHECK(blocks == 2, "a multiple read: %d blocks", blocks);
	xchg(0xff);
	uint8_t p[6] = { 0x40 | 12, 0, 0, 0, 0, 0x01 };
	for (int i = 0; i < 6; i++) xchg(p[i]);
	xchg(0xff);						// the stuff byte
	uint8_t r = 0xff;
	for (int i = 0; i < 10 && (r & 0x80); i++) r = xchg(0xff);
	CHECK(r == 0x00, "CMD12 stops it");

	wr(SPI + 8, 0);
	CHECK(xchg(0x40) == 0xff, "deselected: the card is silent");
	CHECK(rd(SPI + 12) == 0x53504931u, "MAGIC SPI1");
	unlink(path);
}

// -- the RTC and socctl --

static void test_rtc_socctl(void) {
	CHECK(rd(RTC) == 0x5a525443u && rd(RTC + 12) == 0, "the RTC: present, not valid at power-on");
	wr(RTC + 4, 1000);
	s.cycles += 2 * 48000000u;
	CHECK(rd(RTC + 4) == 1002 && rd(RTC + 12) == 1, "two seconds later: %u", rd(RTC + 4));
	wr(RTC + 20, (uint32_t)-60);
	CHECK(rd(RTC + 20) == (uint32_t)-60, "the time zone, signed");

	CHECK(rd(SOC + 4) == 0x5a435452u, "socctl's MAGIC");
	CHECK((rd(SOC + 8) >> 16) == 0x5643, "VIDEO's signature");
	wr(SOC + 8, 3);
	CHECK((rd(SOC + 8) & 3) == 3 && s.video_bright == 7, "paper, and the brightness kept");
	CHECK((rd(SOC + 0x20) >> 16) == 0x5a4b && (rd(SOC + 0x20) & 0x33) == 0x10, "CURSOR: slots 0 and 1");
	wr(SOC + 0x20, 2 | (3 << 4) | (32 << 16));
	wr(SOC + 0x3c, 0xabcd);
	CHECK(s.cursor_rows[32] == 0xabcd && (rd(SOC + 0x20) & 0x33) == 0x32, "a shape row loaded, slots 2 and 3");
	wr(SOC + 0x18, 0x12345678u);
	CHECK(!s.reboot, "RECONFIG ignores anything but its key");
	wr(SOC + 0x18, 0x5a524254u);
	CHECK(s.reboot, "and reconfigures on it");
}

// -- the USB HID ports and the flash's write path --

#define USB  0xc0000000u
#define FLW  0x1f000000u

static void test_hid(void) {
	CHECK((rd(USB) >> 24 & 3) == 1 && (rd(USB + 0x20) >> 24 & 3) == 2, "a keyboard on port 0, a mouse on 1");
	s.irq_pending = 0;
	zsys_key(&s, 0x04, true);
	CHECK(rd(USB + 4) == 0x04000000u && (s.irq_pending & (1u << 5)), "a key down: in the report, IRQ 5");
	zsys_key(&s, 0xe1, true);
	zsys_key(&s, 0x05, true);
	CHECK((rd(USB) & 0xff) == 0x02 && rd(USB + 4) == 0x04050000u, "Shift and a second key");
	zsys_key(&s, 0x04, false);
	CHECK(rd(USB + 4) == 0x05000000u, "the first released: the rest move up");
	uint64_t n = s.hid[0].reports;
	zsys_key(&s, 0xe1, true);
	CHECK(s.hid[0].reports == n, "a modifier already down: no report");
	zsys_key(&s, 0xe1, false); zsys_key(&s, 0x05, false);
	CHECK(rd(USB) == 0x01000000u && rd(USB + 4) == 0, "all up");
	s.irq_pending = 0;
	zsys_mouse(&s, 100, 50, 1);
	CHECK(rd(USB + 0x2c) == (1u << 20 | 50u << 10 | 100u) && (s.irq_pending & (1u << 6)), "the mouse: its cursor register, IRQ 6");
	CHECK((rd(USB + 0x28) & 0xff) == 100 && ((rd(USB + 0x28) >> 8) & 0xff) == 50, "and its movement");
	zsys_wheel(&s, 3);
	zsys_wheel(&s, -1);
	CHECK((rd(USB + 0x28) >> 24) == 2, "the wheel's count, in the mouse register's top byte");
	zsys_mouse(&s, 900, -5, 0);
	CHECK(rd(USB + 0x2c) == (0u << 10 | 639u), "kept on the screen");
	int x, y;
	zsys_pointer(&s, &x, &y);
	CHECK(x == 639 && y == 0, "where the hardware cursor is drawn");
}

static void test_flashw(void) {
	static uint8_t img[2 * 1024 * 1024];
	memset(img, 0xff, sizeof(img));
	CHECK(zsys_load_flash(&s, img, sizeof(img), NULL, NULL), "a 2MB flash");
	CHECK(rd(FLW) == 0x5a464c53u && rd(FLW + 8) == 0xef4015u && rd(FLW + 12) == 0x40000u, "MAGIC, a W25Q16's ID, the lock");
	for (int i = 0; i < 4; i++) zdev_write(&s, FLW + 0x100 + 4 * i, 0x11223344u * (i + 1), 15);
	wr(FLW + 0x10, 0x1fe000); wr(FLW + 0x1c, 16);
	wr(FLW + 0x18, 2);
	CHECK(rd(FLW + 4) & (1u << 3), "a program not armed: refused");
	CHECK(s.flash[0x1fe000] == 0xff, "and nothing written");
	wr(FLW + 0x14, 0x5a465752u); wr(FLW + 0x18, 2);
	CHECK((rd(FLW + 4) & 0x3e) == 2 && s.flash[0x1fe000] == 0x44 && s.flash[0x1fe00f] == 0x44 && s.flash[0x1fe010] == 0xff,
		"armed: 16 bytes programmed, done");
	wr(FLW + 0x18, 2);
	CHECK(rd(FLW + 4) & (1u << 3), "armed for one operation only");
	wr(FLW + 0x14, 0x5a465752u); wr(FLW + 0x10, 0x1fe800); wr(FLW + 0x18, 1);
	CHECK(s.flash[0x1fe000] == 0xff, "an erase: the 4KB sector is 0xFF again");
	wr(FLW + 0x14, 0x5a465752u); wr(FLW + 0x10, 0x3f000); wr(FLW + 0x18, 1);
	CHECK(rd(FLW + 4) & (1u << 2), "below LOCK_END: refused");
	wr(FLW + 0x14, 0x5a465752u); wr(FLW + 0x10, 0x1fe0f8); wr(FLW + 0x1c, 16); wr(FLW + 0x18, 2);
	CHECK(rd(FLW + 4) & (1u << 4), "across a page: refused");
}

// -- audio --

#define AUD 0x70000500u

static void frames(int n) {			// n frame periods of the sample clock
	for (int i = 0; i < n; i++) {
		s.audio.acc += (uint64_t)s.audio.mod * 128u;
		zdev_audio_frames(&s);
	}
}

static void test_audio(void) {
	s.audio_on = true;
	CHECK(rd(AUD) == 0x5a415544u && (rd(AUD + 0x18) >> 16) == 0x5a41, "the audio block: MAGIC, CONFIG's signature");
	CHECK((rd(AUD + 0x3c) & 0xffff) == 147 && (rd(AUD + 0x3c) >> 16) == 1250, "44100 Hz at power-on");
	float out[16];
	while (zsys_audio_take(&s, out, 8, 44100)) ;
	s.audio.r_n = 0; s.audio.r_phase = 0;
	wr(AUD + 4, 1);					// EN: the FIFO path
	wr(AUD + 0x38, 256);				// unity volume
	wr(AUD + 0x0c, (uint32_t)(uint16_t)1000 << 16 | (uint16_t)-1000);
	wr(AUD + 0x0c, (uint32_t)(uint16_t)2000 << 16 | (uint16_t)-2000);
	CHECK((rd(AUD + 8) & 0xffff) == 2, "two frames in the FIFO");
	frames(3);
	CHECK(s.audio.ring[(s.audio.r_head + 0) % ZSYS_AUDIO_OUT][0] == 1000 &&
	      s.audio.ring[(s.audio.r_head + 1) % ZSYS_AUDIO_OUT][1] == -2000, "played in order");
	CHECK(s.audio.ring[(s.audio.r_head + 2) % ZSYS_AUDIO_OUT][0] == 2000 && (rd(AUD + 8) & (1u << 19)),
	      "an empty FIFO holds the last frame, and says UNDERRUN");
	wr(AUD + 4, 1 | 0x10);
	CHECK(!(rd(AUD + 8) & (1u << 19)), "CLRUR clears it");
	wr(AUD + 0x38, 128);				// 50%
	wr(AUD + 0x0c, (uint32_t)(uint16_t)1000 << 16 | 1000);
	frames(1);
	CHECK(s.audio.ring[(s.audio.r_head + s.audio.r_n - 1) % ZSYS_AUDIO_OUT][0] == 500, "the system volume scales it");

	// the mixer: an 8-bit sample of 64 in main memory, gain 255, MIXVOL 128
	uint8_t smp[8] = { 64, 64, 64, 64, 64, 64, 64, 64 };
	memcpy(s.ram + 0x5000, smp, 8);
	wr(AUD + 0x38, 256);
	wr(AUD + 0x40 + 0, 0x40005000u);		// channel 0: BASE, LEN, LOOPST, LOOPLEN, STEP
	wr(AUD + 0x40 + 4, 8);
	wr(AUD + 0x40 + 8, 0);
	wr(AUD + 0x40 + 12, 0);
	wr(AUD + 0x40 + 16, 1u << 14);			// one byte a frame
	wr(AUD + 0x40 + 20, (1u << 17) | (1u << 16) | 255 | 0 << 8);	// TRIG, EN, left only
	wr(AUD + 4, 0x40 | 1);				// MIXEN, EN
	frames(1);
	const int16_t *f = s.audio.ring[(s.audio.r_head + s.audio.r_n - 1) % ZSYS_AUDIO_OUT];
	int want = (64 << 8) * 255 * 128 >> 18;
	CHECK(f[0] == want && f[1] == 0, "the mixer: (sample << 8) gain MIXVOL >> 18 -- %d, %d (want %d, 0)", f[0], f[1], want);
	CHECK(rd(AUD + 0x24) == 1, "MIXSTAT: channel 0 sounding");
	frames(8);
	CHECK(rd(AUD + 0x24) == 0, "a one-shot sample ends");
	s.audio_on = false;
}

// -- the picture: game mode, colour planes, chunky, the colour window --

static uint32_t px_at(int x, int y) { uint32_t row[ZSYS_SCREEN_W]; zsys_render_row(&s, y, row, false); return row[x]; }
static void fbset(uint32_t x, uint32_t y) { s.vram[y * 80 + x / 8] |= (uint8_t)(1u << (x & 7)); }

static void test_picture(void) {
	clear();
	fbset(100, 50);
	CHECK(px_at(100, 50) == 0xffffffffu && px_at(101, 50) == 0xff000000u, "the desktop: a pixel in the ink");
	wr(SOC + 0x0c, 1);					// game mode, the view at (0, 0)
	wr(SOC + 0x10, 0);
	CHECK(px_at(200, 100) == 0xffffffffu && px_at(201, 101) == 0xffffffffu && px_at(202, 100) == 0xff000000u,
		"game mode: the pixel doubled");
	wr(SOC + 0x10, 40 << 16 | 50);
	CHECK(px_at(100, 20) == 0xffffffffu && px_at(200, 100) == 0xff000000u, "the view moves it");
	wr(SOC + 0x10, 400);					// past 320, clamped
	fbset(320, 0);
	CHECK(px_at(0, 0) == 0xffffffffu && (rd(SOC + 0x10) & 0x3ff) == 400,
		"a view past 320 is clamped there at the frame, and reads back as written");
	wr(SOC + 0x10, 0);
	clear();
	fbset(10, 10); fbset(330, 10); fbset(10, 250);		// planes 0, 1 (dx) and 2 (dy)
	fbset(10, 490);						// plane 4: the second sheet
	wr(SOC + 0x24, 1 | (7 << 1) | (1 << 8) | (2 << 10) | (3 << 12));	// eight planes, the 16-colour offsets
	wr(SOC + 0x34, 0x17u << 24 | 0x0f0);			// entry 0x17: green
	CHECK(px_at(20, 20) == 0xff00ff00u, "colour: planes 0, 1, 2 and 4 make index 0x17, through the palette");
	CHECK((rd(SOC + 0x24) >> 4 & 3) == 2 && (rd(SOC + 0x24) & 0x8000), "COLOR: available, eight planes");
	wr(SOC + 0x0c, 0);
	CHECK(!(rd(SOC + 0x24) & 1), "leaving game mode leaves colour");
	clear();
	s.vram[3 * 80 + 5] = 0x42;				// chunky: line 3, pixel 5
	wr(SOC + 0x0c, 1);
	wr(SOC + 0x24, 1 | (7 << 1) | (1 << 14));
	wr(SOC + 0x34, 0x42u << 24 | 0xf00);
	CHECK(px_at(10, 6) == 0xffff0000u, "chunky: a byte a pixel, through the palette");
	wr(SOC + 0x0c, 0);
	clear();
	fbset(5, 5);
	fbset(5, 485);						// its accent bit
	wr(SOC + 0x34, 0x11u << 24 | 0x00f);
	wr(SOC + 0x44, 480 << 16 | 640);
	wr(SOC + 0x40, 0x80000000u | 0x40000000u);		// over the screen, DESK
	CHECK(px_at(5, 5) == 0xff0000ffu && px_at(6, 5) == 0xff000000u,
		"desktop colour: an accent pixel through the palette, the rest the monochrome pixel");
	wr(SOC + 0x0c, 1);
	CHECK(!(rd(SOC + 0x40) & 1), "entering game mode turns the colour window off");
	wr(SOC + 0x0c, 0);
}

int main(void) {
	if (!zsys_init(&s, ZSYS_SOC, 32)) { printf("FAIL: no memory\n"); return 1; }
	zsys_reset(&s);
	test_fill();
	test_glyph();
	test_copy();
	test_raster();
	test_sd();
	test_rtc_socctl();
	zsys_reset(&s);
	test_hid();
	test_flashw();
	zsys_reset(&s);
	test_audio();
	zsys_reset(&s);
	test_picture();
	zsys_free(&s);
	printf("%s: %d checks, %d failed\n", fails ? "FAIL test_zdev" : "PASS test_zdev", checks, fails);
	return fails != 0;
}
