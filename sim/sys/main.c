/*
 * zsys -- the Zeitlos SoC on a host terminal (docs/sim_parity.md).
 *
 *   zsys --flash zeitlos-sim.img                 the sim image: its BIOS is in it
 *   zsys --bios bios.bin --flash zeitlos-lakritz.img
 *                                                a release image, gateware ignored
 *   zsys --tb prog.bin                           a zeitlos32 test program
 *
 * The console is UART0: what the machine prints comes out here, what is
 * typed here goes to it (the terminal in raw mode; Ctrl+] leaves).
 *
 *   --seconds N      stop after N seconds of the machine's time (status 0,
 *                    unless --expect was given and never seen)
 *   --expect TEXT    stop, with status 0, once the console has printed TEXT
 *                    (1 if the time runs out first)
 *   --type TEXT      type TEXT at the console once --after has been printed
 *                    (\n is Enter)
 *   --after TEXT     (default: the first --expect, or nothing: at once)
 *   --fast           do not keep to the board's pace: as fast as the host can
 *   --cpi N          clocks charged per instruction (default 2)
 *   --ram MB         main memory (default 32)
 *   --stats          instructions, the machine's time and the host's, at the end
 *   --stamps         the machine's time, in seconds, before each line printed
 *   --profile T      from T seconds on, where the CPU was at each tick, at the end
 *   --sd FILE        an SD card: this image, written through as the machine writes
 *   --snapshot       the card's writes kept in memory, the file left alone
 *   --screenshot F   the screen as a PNG when zsys stops (--cursor: with the pointer)
 *   --clock          the RTC starts at the host's time, as if the board kept one
 *   --headless       the board without its GPU, SD card or RTC (milestone 1's)
 *   --window         a window: the screen, the keyboard and the mouse (SDL2)
 *   --scale N        its size, in host pixels a board pixel (default 2)
 *   --keys TEXT      type TEXT on the USB keyboard once --after has been printed
 *                    (\n Enter, \t Tab, \e Esc, \b Backspace; \{F1}, \{Super+S}, ...)
 *   --click X,Y      move the mouse there and click, after the keys before it
 *   --wheel N        turn the mouse's wheel N notches (up positive), in order with the rest
 *   --wait MS        between scripted inputs, MS of the machine's time
 *   --wav FILE       what the machine played, as a WAV at its own rate (16-bit stereo)
 *   --mute           the window plays nothing
 *   --fullscreen     the window full screen (Ctrl+Alt+Enter toggles it)
 *
 * The flash image is written as the machine writes the flash (the
 * key/value store), as the card image is -- unless --snapshot.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include "zsys.h"
#ifdef ZSYS_WINDOW
#include "win.h"
#endif

#define SYSCLK_HZ 48000000u

// sw/common/zsoc.h's Z_FEATURE_* bits, the ones this board has
#define Z_MEM_SDRAM     (1u << 1)
#define Z_MEM_VRAM      (1u << 2)
#define Z_MEM_ROM       (1u << 4)
#define Z_MEM_GLYPH     (1u << 5)
#define Z_GPU           (1u << 6)
#define Z_GPU_RASTER    (1u << 7)
#define Z_GPU_BLIT      (1u << 8)
#define Z_GPU_CURSOR    (1u << 9)
#define Z_UART0         (1u << 12)
#define Z_USB_HID       (1u << 13)
#define Z_SPI_SDCARD    (1u << 14)
#define Z_CPU_MUL       (1u << 20)
#define Z_CPU_DIV       (1u << 21)
#define Z_CPU_ZEITLOS32 (1u << 23)
#define Z_RTC           (1u << 24)
#define Z_AUDIO         (1u << 26)
#define Z_GAME          (1u << 27)
#define Z2_COLOR        (1u << 15)
#define Z2_RECONFIG     (1u << 5)
#define Z2_FLASHW       (1u << 6)

// -- scripted input: keys and clicks on the USB devices, in order --

typedef struct { uint8_t kind, usage, mods; int16_t x, y; } ev_t;	// kind: 0 key, 1 click, 2 wait, 3 wheel
static ev_t script[4096];
static int script_n, script_at;
static uint32_t script_wait_ms = 40;

static void ev_add(ev_t e) { if (script_n < (int)(sizeof(script) / sizeof(script[0]))) script[script_n++] = e; }

// a US keyboard's usage for a character, with Shift if it needs it:
// usages 30 to 56, unshifted and shifted (0: no such character)
static const char us_plain[27] = { '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '\n', 0x1b, '\b', '\t', ' ',
	'-', '=', '[', ']', '\\', 0, ';', '\'', '`', ',', '.', '/' };
static const char us_shift[27] = { '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', 0, 0, 0, 0, 0,
	'_', '+', '{', '}', '|', 0, ':', '"', '~', '<', '>', '?' };
static bool ascii_usage(char c, uint8_t *u, bool *shift) {
	*shift = false;
	if (c >= 'a' && c <= 'z') { *u = (uint8_t)(4 + c - 'a'); return true; }
	if (c >= 'A' && c <= 'Z') { *u = (uint8_t)(4 + c - 'A'); *shift = true; return true; }
	for (int i = 0; i < 27 && c; i++) {
		if (us_plain[i] == c) { *u = (uint8_t)(30 + i); return true; }
		if (us_shift[i] == c) { *u = (uint8_t)(30 + i); *shift = true; return true; }
	}
	return false;
}

static const struct { const char *name; uint8_t usage; } key_names[] = {
	{ "Enter", 0x28 }, { "Esc", 0x29 }, { "Backspace", 0x2a }, { "Tab", 0x2b }, { "Space", 0x2c },
	{ "F1", 0x3a }, { "F2", 0x3b }, { "F3", 0x3c }, { "F4", 0x3d }, { "F5", 0x3e }, { "F6", 0x3f },
	{ "F7", 0x40 }, { "F8", 0x41 }, { "F9", 0x42 }, { "F10", 0x43 }, { "F11", 0x44 }, { "F12", 0x45 },
	{ "Insert", 0x49 }, { "Home", 0x4a }, { "PageUp", 0x4b }, { "Delete", 0x4c }, { "End", 0x4d },
	{ "PageDown", 0x4e }, { "Right", 0x4f }, { "Left", 0x50 }, { "Down", 0x51 }, { "Up", 0x52 },
};

// "\{Ctrl+Alt+Delete}": modifiers (Ctrl Shift Alt Super) and one key
static bool parse_combo(const char *t, size_t n, uint8_t *u, uint8_t *mods) {
	*mods = 0;
	for (;;) {
		const char *plus = memchr(t, '+', n);
		if (!plus || plus == t + n - 1) break;
		size_t m = (size_t)(plus - t);
		if (m == 4 && !strncmp(t, "Ctrl", 4)) *mods |= 0x01;
		else if (m == 5 && !strncmp(t, "Shift", 5)) *mods |= 0x02;
		else if (m == 3 && !strncmp(t, "Alt", 3)) *mods |= 0x04;
		else if (m == 5 && !strncmp(t, "Super", 5)) *mods |= 0x08;
		else return false;
		n -= m + 1;
		t = plus + 1;
	}
	for (size_t i = 0; i < sizeof(key_names) / sizeof(key_names[0]); i++)
		if (strlen(key_names[i].name) == n && !strncmp(key_names[i].name, t, n)) { *u = key_names[i].usage; return true; }
	if (n == 1) {
		bool sh;
		char c = t[0] >= 'A' && t[0] <= 'Z' ? (char)(t[0] - 'A' + 'a') : t[0];
		if (ascii_usage(c, u, &sh)) { if (sh) *mods |= 0x02; return true; }
	}
	return false;
}

static bool script_keys(const char *v) {
	for (const char *r = v; *r; r++) {
		ev_t e = { 0, 0, 0, 0, 0 };
		bool sh = false;
		if (*r == '\\' && r[1] == '{') {
			const char *end = strchr(r, '}');
			if (!end || !parse_combo(r + 2, (size_t)(end - r - 2), &e.usage, &e.mods)) {
				fprintf(stderr, "zsys: --keys: no key %.*s\n", end ? (int)(end - r + 1) : 8, r);
				return false;
			}
			r = end;
		} else if (*r == '\\' && r[1]) {
			r++;
			char c = *r == 'n' ? '\n' : *r == 't' ? '\t' : *r == 'e' ? 0x1b : *r == 'b' ? '\b' : *r;
			if (!ascii_usage(c, &e.usage, &sh)) return false;
		} else if (!ascii_usage(*r, &e.usage, &sh)) {
			fprintf(stderr, "zsys: --keys: no key for '%c' on a US keyboard\n", *r);
			return false;
		}
		if (sh) e.mods |= 0x02;
		ev_add(e);
	}
	return true;
}

// one step of the script; true while there is more to do
static uint64_t script_next;			// the machine's cycle for the next step
static int script_phase;
static bool script_step(zsys_t *s) {
	if (script_at >= script_n) return false;
	if (s->cycles < script_next) return true;
	const ev_t *e = &script[script_at];
	uint64_t gap = (uint64_t)script_wait_ms * (SYSCLK_HZ / 1000);
	if (e->kind == 0) {				// modifiers down, key down, key up, modifiers up
		static const uint8_t mod_usage[4] = { 0xe0, 0xe1, 0xe2, 0xe3 };
		if (script_phase == 0) {
			for (int b = 0; b < 4; b++) if (e->mods & (1 << b)) zsys_key(s, mod_usage[b], true);
			zsys_key(s, e->usage, true);
			script_phase = 1;
			script_next = s->cycles + gap / 2;
			return true;
		}
		zsys_key(s, e->usage, false);
		for (int b = 0; b < 4; b++) if (e->mods & (1 << b)) zsys_key(s, mod_usage[b], false);
	} else if (e->kind == 1) {			// move, press, release
		if (script_phase == 0) { zsys_mouse(s, e->x, e->y, 0); script_phase = 1; script_next = s->cycles + gap / 2; return true; }
		if (script_phase == 1) { zsys_mouse(s, e->x, e->y, 1); script_phase = 2; script_next = s->cycles + gap / 2; return true; }
		zsys_mouse(s, e->x, e->y, 0);
	} else if (e->kind == 3) {			// the wheel, at the pointer as it is
		zsys_wheel(s, e->x);
	} else {					// a wait
		if (script_phase == 0) { script_phase = 1; script_next = s->cycles + (uint64_t)e->x * (SYSCLK_HZ / 1000); return true; }
	}
	script_phase = 0;
	script_at++;
	script_next = s->cycles + gap;
	return script_at < script_n;
}

static char seen[8192];			// the last of what the console printed
static size_t seen_n;

static bool stamps;			// --stamps: the machine's time on each line
static bool at_line_start = true;

static void out(zsys_t *s, uint8_t c) {
	if (stamps && at_line_start) printf("[%9.3f] ", (double)s->cycles / 48e6);
	at_line_start = c == '\n';
	fputc(c, stdout);
	if (c == '\n') fflush(stdout);
	if (seen_n == sizeof(seen) - 1) {
		memmove(seen, seen + 4096, seen_n - 4096);
		seen_n -= 4096;
	}
	seen[seen_n++] = (char)c;
	seen[seen_n] = 0;
}

// --profile: at each tick, which process (the MTU's base) and which 4KB
// of code. What a stalled machine is doing.
#define PROF_N 4096
static struct { uint32_t mtu, page, n; } prof[PROF_N];
static double prof_from = -1;
static void on_tick(zsys_t *s) {
	if ((double)s->cycles / 48e6 < prof_from) return;
	uint32_t page = s->pc >> 12, h = (s->mtu * 31u + page) % PROF_N;
	for (int k = 0; k < PROF_N; k++, h = (h + 1) % PROF_N) {
		if (prof[h].n && (prof[h].mtu != s->mtu || prof[h].page != page)) continue;
		prof[h].mtu = s->mtu; prof[h].page = page; prof[h].n++;
		return;
	}
}
static int prof_cmp(const void *a, const void *b) {
	return (int)((const typeof(prof[0]) *)b)->n - (int)((const typeof(prof[0]) *)a)->n;
}

static uint8_t *slurp(const char *path, uint32_t *len) {
	FILE *f = fopen(path, "rb");
	if (!f) { perror(path); return NULL; }
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *p = malloc((size_t)n + 1);
	if (!p || fread(p, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(p); return NULL; }
	fclose(f);
	*len = (uint32_t)n;
	return p;
}

// -- --wav: the machine's audio as it played, at its own rate --
static FILE *wav;
static uint32_t wav_frames, wav_hz;
static void wav_header(FILE *f, uint32_t hz, uint32_t frames) {
	uint32_t data = frames * 4;
	uint8_t h[44] = { 'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ', 16,0,0,0, 1,0, 2,0,
		0,0,0,0, 0,0,0,0, 4,0, 16,0, 'd','a','t','a', 0,0,0,0 };
	uint32_t riff = 36 + data, rate = hz, bps = hz * 4;
	memcpy(h + 4, &riff, 4); memcpy(h + 24, &rate, 4); memcpy(h + 28, &bps, 4); memcpy(h + 40, &data, 4);
	fseek(f, 0, SEEK_SET);
	fwrite(h, 1, 44, f);
	fseek(f, 0, SEEK_END);
}
static void wav_take(zsys_t *s) {
	static float buf[2 * 4096];
	uint32_t hz = (uint32_t)(zsys_audio_hz(s) + 0.5), n;
	if (!wav_hz) wav_hz = hz;
	while ((n = zsys_audio_take(s, buf, 4096, wav_hz)) > 0)
		for (uint32_t i = 0; i < 2 * n; i++) {
			float v = buf[i] * 32768.0f;
			int16_t o = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
			fwrite(&o, 2, 1, wav);
			if (i & 1) wav_frames++;
		}
}

static struct termios saved_tio;
static bool raw_on;
static void raw_off(void) { if (raw_on) tcsetattr(0, TCSANOW, &saved_tio); raw_on = false; }
static void raw_mode(void) {
	if (!isatty(0)) return;
	struct termios t;
	tcgetattr(0, &saved_tio);
	t = saved_tio;
	cfmakeraw(&t);
	t.c_oflag |= OPOST | ONLCR;
	tcsetattr(0, TCSANOW, &t);
	raw_on = true;
	atexit(raw_off);
	fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);
}

static double now(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void usage(void) {
	fprintf(stderr, "usage: zsys [--bios FILE] --flash FILE | --tb PROG.bin  [--seconds N] [--expect TEXT]\n"
		"            [--type TEXT] [--after TEXT] [--fast] [--cpi N] [--ram MB] [--stats]\n"
		"            [--sd FILE] [--snapshot] [--screenshot FILE [--cursor]] [--clock] [--headless]\n"
		"            [--window [--scale N]] [--keys TEXT] [--click X,Y] [--wheel N] [--wait MS] [--wav FILE] [--mute] [--fullscreen]\n");
	exit(2);
}

int main(int argc, char **argv) {

	const char *bios_path = NULL, *flash_path = NULL, *tb_path = NULL;
	const char *expect = NULL, *type = NULL, *after = NULL;
	double seconds = 0;
	bool fast = false, stats = false, snapshot = false, cursor = false, clock = false, headless = false;
	bool window = false, mute = false, full = false;
	const char *wav_path = NULL;
	int scale = 2;
	const char *sd_path = NULL, *shot_path = NULL;
	uint32_t cpi = 2, ram_mb = 32;

	for (int i = 1; i < argc; i++) {
		const char *o = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
		if (!strcmp(o, "--bios") && v) { bios_path = v; i++; }
		else if (!strcmp(o, "--flash") && v) { flash_path = v; i++; }
		else if (!strcmp(o, "--tb") && v) { tb_path = v; i++; }
		else if (!strcmp(o, "--seconds") && v) { seconds = atof(v); i++; }
		else if (!strcmp(o, "--expect") && v) { expect = v; i++; }
		else if (!strcmp(o, "--type") && v) {
			// \n is Enter, \t Tab, \\ a backslash, so a script can type a line
			char *t = strdup(v), *w = t;
			for (const char *r = v; *r; r++) {
				if (*r == '\\' && r[1]) {
					r++;
					*w++ = *r == 'n' ? '\n' : *r == 't' ? '\t' : *r;
				} else *w++ = *r;
			}
			*w = 0;
			type = t;
			i++;
		}
		else if (!strcmp(o, "--after") && v) { after = v; i++; }
		else if (!strcmp(o, "--cpi") && v) { cpi = (uint32_t)atoi(v); i++; }
		else if (!strcmp(o, "--ram") && v) { ram_mb = (uint32_t)atoi(v); i++; }
		else if (!strcmp(o, "--fast")) fast = true;
		else if (!strcmp(o, "--stats")) stats = true;
		else if (!strcmp(o, "--stamps")) stamps = true;
		else if (!strcmp(o, "--profile") && v) { prof_from = atof(v); i++; }
		else if (!strcmp(o, "--sd") && v) { sd_path = v; i++; }
		else if (!strcmp(o, "--snapshot")) snapshot = true;
		else if (!strcmp(o, "--screenshot") && v) { shot_path = v; i++; }
		else if (!strcmp(o, "--cursor")) cursor = true;
		else if (!strcmp(o, "--clock")) clock = true;
		else if (!strcmp(o, "--headless")) headless = true;
		else if (!strcmp(o, "--window")) window = true;
		else if (!strcmp(o, "--wav") && v) { wav_path = v; i++; }
		else if (!strcmp(o, "--mute")) mute = true;
		else if (!strcmp(o, "--fullscreen")) { full = true; window = true; }
		else if (!strcmp(o, "--scale") && v) { scale = atoi(v); if (scale < 1) scale = 1; i++; }
		else if (!strcmp(o, "--keys") && v) { if (!script_keys(v)) return 2; i++; }
		else if (!strcmp(o, "--click") && v) {
			ev_t e = { 1, 0, 0, 0, 0 };
			int x, y;
			if (sscanf(v, "%d,%d", &x, &y) != 2) usage();
			e.x = (int16_t)x; e.y = (int16_t)y;
			ev_add(e);
			i++;
		}
		else if (!strcmp(o, "--wheel") && v) { ev_t e = { 3, 0, 0, (int16_t)atoi(v), 0 }; ev_add(e); i++; }
		else if (!strcmp(o, "--wait") && v) { ev_t e = { 2, 0, 0, (int16_t)atoi(v), 0 }; ev_add(e); i++; }
		else usage();
	}

	static zsys_t s;
	uint32_t len;
	uint8_t *data;

	// -- a test program: tb_zeitlos32.v's machine --
	if (tb_path) {
		if (!zsys_init(&s, ZSYS_TB, 0) || !(data = slurp(tb_path, &len)) || !zsys_load_bram(&s, data, len))
			return 2;
		s.uart_out = out;
		s.cpi = cpi ? cpi : 1;
		zsys_reset(&s);
		int r = zsys_run(&s, seconds > 0 ? (uint64_t)(seconds * SYSCLK_HZ) : 50000000u);
		fflush(stdout);
		if (r == ZSYS_TB_DONE && s.tb_result == 0) { printf("PASS %s (%llu instructions)\n", tb_path,
			(unsigned long long)s.instret); return 0; }
		if (r == ZSYS_TB_DONE) printf("FAIL %s: check %lu\n", tb_path, (unsigned long)s.tb_result);
		else if (r == ZSYS_TRAPPED) printf("FAIL %s: trapped at %08lx (%s)\n", tb_path,
			(unsigned long)s.trap_pc, s.trap_why);
		else printf("FAIL %s: %s at pc %08lx\n", tb_path, r == ZSYS_IDLE ? "idle forever" : "out of time",
			(unsigned long)s.pc);
		return 1;
	}

	// -- the board --
	if (!flash_path) usage();
	if (!zsys_init(&s, ZSYS_SOC, ram_mb)) { fprintf(stderr, "zsys: out of memory\n"); return 2; }
	s.cpi = cpi ? cpi : 1;
	s.uart_out = out;

	// A Lakritz without its USB host (docs/zsys.md, "The board"): memory,
	// the flash window, glyph memory, the GPU's rasterizer, blitter and
	// cursor, UART0, the SD card's SPI controller, the RTC, the CPU's
	// multiply and divide. --headless: milestone 1's board, as Klinge
	// is, without the GPU, the card or the RTC.
	s.features = Z_MEM_SDRAM | Z_MEM_VRAM | Z_MEM_ROM | Z_UART0 | Z_CPU_MUL | Z_CPU_DIV | Z_CPU_ZEITLOS32;
	if (!headless)
		s.features |= Z_MEM_GLYPH | Z_GPU | Z_GPU_RASTER | Z_GPU_BLIT | Z_GPU_CURSOR | Z_USB_HID |
			Z_SPI_SDCARD | Z_RTC | Z_AUDIO | Z_GAME;
	s.audio_on = !headless;
	s.features2 = headless ? 0 : Z2_RECONFIG | Z2_FLASHW | Z2_COLOR;
	if (sd_path && !zsys_sd_insert(&s, sd_path, snapshot)) return 2;

	if (!(data = slurp(flash_path, &len))) return 2;
	const uint8_t *bios = NULL;
	uint32_t bios_len = 0;
	if (!zsys_load_flash(&s, data, len, &bios, &bios_len)) return 2;
	free(data);
	if (!snapshot && !headless) {			// the flash's writes reach its file
		int fd = open(flash_path, O_RDWR);
		if (fd < 0) fprintf(stderr, "zsys: %s is read-only: the flash's writes stay in memory\n", flash_path);
		else zsys_flash_file(&s, fd);
	}
	if (bios_path) {
		uint8_t *b = slurp(bios_path, &bios_len);
		if (!b || !zsys_load_bram(&s, b, bios_len)) { fprintf(stderr, "zsys: %s: no BIOS\n", bios_path); return 2; }
		free(b);
	} else if (bios) {
		if (!zsys_load_bram(&s, bios, bios_len)) { fprintf(stderr, "zsys: the image's BIOS is too large\n"); return 2; }
	} else {
		fprintf(stderr, "zsys: %s has no BIOS in it (not a sim image): give one with --bios\n", flash_path);
		return 2;
	}

	if (clock) s.rtc_host = true;
	zsys_reset(&s);
	if (prof_from >= 0) s.on_tick = on_tick;
	raw_mode();
	if (wav_path) {
		if (!(wav = fopen(wav_path, "wb"))) { perror(wav_path); return 2; }
		wav_header(wav, 44100, 0);
	}
#ifdef ZSYS_WINDOW
	double last_frame = 0;
	if (window && !win_open(scale, "zsys -- Zeitlos", !mute)) return 2;
	if (window && full) win_fullscreen(true);
#else
	if (window) { fprintf(stderr, "zsys: built without SDL2: no --window (docs/zsys.md)\n"); return 2; }
#endif

	if (!after) after = (type || script_n) ? expect : NULL;
	bool scripted = script_n == 0, script_seen = false;
	bool typed = !type;
	const char *tp = type;
	uint64_t limit = seconds > 0 ? (uint64_t)(seconds * SYSCLK_HZ) : 0;
	double t0 = now();
	int result = 1;

	for (;;) {

		// 1ms of the machine's time a slice
		int r = zsys_run(&s, SYSCLK_HZ / 1000);
		if (r == ZSYS_TRAPPED) {
			raw_off();
			fprintf(stderr, "\nzsys: the core halted at %08lx: %s\n", (unsigned long)s.trap_pc, s.trap_why);
			result = 3;
			break;
		}

		if (expect && strstr(seen, expect) && typed && scripted) { result = 0; break; }
		if (!typed && (!after || strstr(seen, after))) {
			if (*tp && zsys_uart_in(&s, (uint8_t)(*tp == '\n' ? '\r' : *tp))) tp++;
			if (!*tp) { typed = true; seen_n = 0; seen[0] = 0; }
		}
		if (wav) wav_take(&s);

		// the keyboard and mouse script, once --after has been printed
		if (!scripted && (!after || script_seen || strstr(seen, after))) {
			script_seen = true;
			if (!script_step(&s)) scripted = true;
		}

#ifdef ZSYS_WINDOW
		if (window) {
			if (!win_poll(&s)) { result = 0; break; }
			double t = now();
			if (t - last_frame >= 1.0 / 60) { win_present(&s); last_frame = t; }
		}
#endif

		uint8_t c;
		while (raw_on && read(0, &c, 1) == 1) {
			if (c == 0x1d) { result = 0; goto done; }	// Ctrl+]
			zsys_uart_in(&s, c);
		}

		if (limit && s.cycles >= limit) { if (!expect) result = 0; break; }	// the time given: done

		// keep to the board's pace unless told not to
		if (!fast) {
			double ahead = (double)s.cycles / SYSCLK_HZ - (now() - t0);
			if (ahead > 0.002) usleep((useconds_t)(ahead * 1e6));
		}

	}

done:
	raw_off();
	if (wav) {
		wav_take(&s);
		wav_header(wav, wav_hz ? wav_hz : 44100, wav_frames);
		fclose(wav);
	}
#ifdef ZSYS_WINDOW
	if (window) win_close();
#endif
	fflush(stdout);
	if (shot_path && !zsys_screenshot(&s, shot_path, cursor)) {
		fprintf(stderr, "zsys: %s: could not write the screenshot\n", shot_path);
		if (!result) result = 2;
	}
	if (stats) {
		double host = now() - t0;
		fprintf(stderr, "\nzsys: %llu instructions, %.2fs of the machine's time in %.2fs: %.1f million a second\n",
			(unsigned long long)s.instret, (double)s.cycles / SYSCLK_HZ, host, s.instret / host / 1e6);
		fprintf(stderr, "zsys: interrupts delivered:");
		for (int i = 0; i < 32; i++) if (s.irq_count[i]) fprintf(stderr, " %d:%llu", i, (unsigned long long)s.irq_count[i]);
		fprintf(stderr, "; waitirq %llu, timer %llu\n", (unsigned long long)s.waitirqs, (unsigned long long)s.timers);
		fprintf(stderr, "zsys: blits %llu, lines %llu; card blocks read %llu, written %llu; reboots %llu\n",
			(unsigned long long)s.blit.blits, (unsigned long long)s.raster.lines,
			(unsigned long long)s.sd.reads, (unsigned long long)s.sd.writes, (unsigned long long)s.reboots);
		fprintf(stderr, "zsys: audio frames %llu at %.0f Hz\n", (unsigned long long)s.audio.frames, zsys_audio_hz(&s));
		fprintf(stderr, "zsys: keyboard reports %llu, mouse %llu; flash sectors erased %llu, pages programmed %llu\n",
			(unsigned long long)s.hid[0].reports, (unsigned long long)s.hid[1].reports,
			(unsigned long long)s.flashw.erases, (unsigned long long)s.flashw.programs);
	}
	if (prof_from >= 0) {
		qsort(prof, PROF_N, sizeof(prof[0]), prof_cmp);
		fprintf(stderr, "zsys: ticks by process (MTU base) and 4KB of code:\n");
		for (int k = 0; k < 12 && prof[k].n; k++)
			fprintf(stderr, "  %6lu  mtu %08lx  pc %05lx000\n", (unsigned long)prof[k].n,
				(unsigned long)prof[k].mtu, (unsigned long)prof[k].page);
	}
	if (expect && result == 1) fprintf(stderr, "\nzsys: never printed \"%s\"\n", expect);
	return result;

}
