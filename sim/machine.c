#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/select.h>

#include "machine.h"

/* ------------------------------------------------------------------- */
/* z_obj_t layout (sw/common/zobj.h): { int32 type; union { ... } val; }
 * On a 32-bit target this is 8 bytes: type at +0, val at +4. */
#define ZOBJ_VAL_OFFSET 4

/* z_syscall_id_t values (sw/common/zeitlos.h + syscalls.def), in order:
 * Z_SYSCALL_NONE=0, then EXIT, UI_PRINT, UART_GETC, UART_PUTC,
 * UART_RX_EMPTY, UART_TX_FULL. */
enum {
	ZSYS_NONE = 0,
	ZSYS_EXIT,
	ZSYS_UI_PRINT,
	ZSYS_UART_GETC,
	ZSYS_UART_PUTC,
	ZSYS_UART_RX_EMPTY,
	ZSYS_UART_TX_FULL,
};

/* ------------------------------------------------------------------- */
/* raw terminal mode so getch()/readline()-style apps get characters
 * immediately, matching a real UART's byte-at-a-time behavior          */

static struct termios g_saved_termios;
static int g_termios_saved = 0;

static void uart_enter_raw(void) {
	if (!isatty(STDIN_FILENO)) return;
	if (tcgetattr(STDIN_FILENO, &g_saved_termios) != 0) return;
	g_termios_saved = 1;
	struct termios raw = g_saved_termios;
	raw.c_lflag &= ~(ICANON | ECHO);
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 0;
	tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

static void uart_leave_raw(void) {
	if (g_termios_saved) tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_termios);
}

static int uart_stdin_has_byte(uart_t *u) {
	if (u->have_pending) return 1;
	fd_set fds;
	struct timeval tv = {0, 0};
	FD_ZERO(&fds);
	FD_SET(STDIN_FILENO, &fds);
	if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0) {
		unsigned char c;
		if (read(STDIN_FILENO, &c, 1) == 1) {
			u->have_pending = 1;
			u->pending_byte = c;
			return 1;
		}
	}
	return 0;
}

static int uart_stdin_getc(uart_t *u) {
	if (!u->have_pending) {
		if (!uart_stdin_has_byte(u)) return -1;
	}
	u->have_pending = 0;
	return u->pending_byte;
}

/* ------------------------------------------------------------------- */
/* line rasterizer -- direct translation of rtl/gpu/gpu_raster.v's
 * Bresenham FSM into a single host-side function.                     */

static void raster_set_pixel(machine_t *m, int x, int y, int color) {
	uint32_t bit = (uint32_t)(y * ZS_SCREEN_W + x);
	uint32_t word = bit / 32, mask = 1u << (bit % 32);
	if (color) m->vram[word] |= mask; else m->vram[word] &= ~mask;
}

static void raster_run(machine_t *m) {
	raster_t *r = &m->raster;

	int x0 = (int)r->x0, y0 = (int)r->y0, x1 = (int)r->x1, y1 = (int)r->y1;

	int deltax = x1 - x0;
	int right = deltax >= 0;
	int dx = right ? deltax : -deltax;

	int deltay = y1 - y0;
	int down = deltay >= 0;
	int dy = down ? -deltay : deltay; /* always <= 0, matches gpu_raster.v */

	int err = dx + dy;
	int cur_x = x0, cur_y = y0;
	uint32_t pixel_count = 0;

	for (;;) {
		int in_clip = !r->clip_enable ||
			((uint32_t)cur_x >= r->clip_x0 && (uint32_t)cur_x <= r->clip_x1 &&
			 (uint32_t)cur_y >= r->clip_y0 && (uint32_t)cur_y <= r->clip_y1);

		if (in_clip && cur_x >= 0 && cur_x < ZS_SCREEN_W &&
		                cur_y >= 0 && cur_y < ZS_SCREEN_H)
			raster_set_pixel(m, cur_x, cur_y, (int)(r->color & 1));

		pixel_count++;
		r->cur_x = (uint32_t)cur_x;
		r->cur_y = (uint32_t)cur_y;

		if ((cur_x == x1 && cur_y == y1) || pixel_count > 1000) break;

		int e2 = err << 1;
		if (e2 > dy) { err += dy; cur_x += right ? 1 : -1; }
		if (e2 < dx) { err += dx; cur_y += down ? 1 : -1; }
	}

	r->pixel_count = pixel_count;
}

/* ------------------------------------------------------------------- */
/* blitter -- direct translation of rtl/gpu/gpu_blit.v.
 *
 * NOTE: "copy" mode is intentionally a no-op here, matching the real
 * RTL as it stands today (see gpu_blit.v: "Copy mode - would need
 * source logic" -- tracked as machdyne/zeitlos issue #3). This keeps
 * the simulator faithful to current hardware behavior rather than
 * quietly fixing a bug the real board doesn't have fixed yet. */

static void blit_run(machine_t *m) {
	blit_t *b = &m->blit;

	uint32_t final_x = b->dst_x;
	uint32_t final_y = b->dst_y;
	uint32_t rect_x_end = b->dst_x + b->width;
	uint32_t rect_y_end = b->dst_y + b->height;
	uint32_t final_x_end = rect_x_end > (uint32_t)ZS_SCREEN_W ? (uint32_t)ZS_SCREEN_W : rect_x_end;
	uint32_t final_y_end = rect_y_end > (uint32_t)ZS_SCREEN_H ? (uint32_t)ZS_SCREEN_H : rect_y_end;

	uint32_t words_per_line, total_lines, line_start_addr;
	uint32_t left_mask, right_mask;
	const uint32_t stride = 64; /* bytes/line: 512/8 */

	if (b->clip_enable) {
		uint32_t final_width = final_x_end - final_x;
		uint32_t final_height = final_y_end - final_y;

		if (final_width == 0 || final_height == 0 ||
		    final_x >= (uint32_t)ZS_SCREEN_W || final_y >= (uint32_t)ZS_SCREEN_H)
			return; /* fully clipped away */

		uint32_t left_word_boundary = (final_x >> 5) << 5;
		uint32_t right_word_boundary = ((final_x_end + 31) >> 5) << 5;
		uint32_t word_span_words = (right_word_boundary - left_word_boundary) >> 5;

		uint32_t left_pixel_start = final_x - left_word_boundary;
		uint32_t right_pixel_end = final_x_end - ((final_x_end >> 5) << 5);

		left_mask = 0xFFFFFFFFu << left_pixel_start;
		right_mask = (right_pixel_end == 0) ? 0xFFFFFFFFu : (0xFFFFFFFFu >> (32 - right_pixel_end));

		words_per_line = word_span_words;
		total_lines = final_height;
		line_start_addr = final_y * stride + (left_word_boundary >> 3);
	} else {
		left_mask = 0xFFFFFFFFu;
		right_mask = 0xFFFFFFFFu;
		words_per_line = (b->width + 31) >> 5;
		total_lines = b->height;
		line_start_addr = b->dst_y * stride + (b->dst_x >> 5) * 4;
	}

	if ((uint64_t)words_per_line * total_lines > 200000ull) {
		fprintf(stderr, "zeitlos-sim: blit request too large, ignoring "
			"(dst=%u,%u w=%u h=%u)\n", b->dst_x, b->dst_y, b->width, b->height);
		return;
	}

	uint32_t addr = line_start_addr;
	for (uint32_t line = 0; line < total_lines; line++) {
		uint32_t word_addr = addr;
		for (uint32_t wi = 0; wi < words_per_line; wi++) {
			uint32_t vram_word_idx = word_addr / 4;
			if (vram_word_idx >= ZS_VRAM_WORDS) { word_addr += 4; continue; }
			uint32_t read_data = m->vram[vram_word_idx];
			uint32_t out;

			if (b->fill) {
				if (b->clip_enable && words_per_line > 1) {
					if (wi == 0)
						out = (read_data & ~left_mask) | (b->pattern & left_mask);
					else if (wi == words_per_line - 1)
						out = (read_data & ~right_mask) | (b->pattern & right_mask);
					else
						out = b->pattern;
				} else if (b->clip_enable && words_per_line == 1) {
					uint32_t both = left_mask & right_mask;
					out = (read_data & ~both) | (b->pattern & both);
				} else {
					out = b->pattern;
				}
			} else {
				out = read_data; /* copy mode: unimplemented in real HW, see above */
			}

			m->vram[vram_word_idx] = out;
			word_addr += 4;
		}
		addr += stride;
	}
}

/* ------------------------------------------------------------------- */
/* syscall gate -- reg_kernel points at ZS_SYSCALL_TRAP_PC; when the CPU
 * lands there (see machine_run) we perform the syscall here in host
 * code and return via ra, exactly matching the ABI in sw/os/kernel.c's
 * z_kernel_entry(): a0=syscall_id, a1=obj ptr, a2=irqs (unused here). */

static void do_syscall(machine_t *m) {
	uint32_t id  = m->cpu.regs[10]; /* a0 */
	uint32_t obj = m->cpu.regs[11]; /* a1 */

	switch (id) {

	case ZSYS_EXIT:
		m->exit_requested = 1;
		m->exit_code = 0;
		break;

	case ZSYS_UART_PUTC: {
		int32_t c = (int32_t)bus_read32(m, obj + ZOBJ_VAL_OFFSET);
		putchar((int)c);
		fflush(stdout);
		break;
	}

	case ZSYS_UART_GETC: {
		int c = uart_stdin_getc(&m->uart);
		bus_write32(m, obj + ZOBJ_VAL_OFFSET, (uint32_t)(int32_t)c);
		break;
	}

	case ZSYS_UART_RX_EMPTY: {
		int empty = !uart_stdin_has_byte(&m->uart);
		bus_write32(m, obj + ZOBJ_VAL_OFFSET, (uint32_t)empty);
		break;
	}

	case ZSYS_UART_TX_FULL:
		bus_write32(m, obj + ZOBJ_VAL_OFFSET, 0); /* host stdout never "full" */
		break;

	case ZSYS_UI_PRINT:
		/* obj is a z_obj_t*; if it's a string object, print it. We don't
		 * have the real z_type_t enum values pinned down here, so this
		 * is best-effort and safe to skip if unsure. */
		break;

	default:
		fprintf(stderr, "zeitlos-sim: unimplemented syscall id=%u\n", id);
		break;
	}

	/* return value convention: pointer to a result object; NULL is fine
	 * for calls apps don't actually inspect the return value of. */
	m->cpu.regs[10] = 0;
}

/* ------------------------------------------------------------------- */
/* bus dispatch */

/* -- GPIO (rtl/gpio.v) --------------------------------------------
 *
 * Enough of the block to run sw/common/zgpio.c and the bit-bang I2C
 * and SPI libraries on top of it, without a board.
 *
 * THE PIN MODEL IS THE WHOLE POINT and is worth stating plainly,
 * because a simulator that gets this wrong is worse than no simulator:
 * it lets code pass that will fail on hardware. A pin reads as
 *
 *   driven by us            -> gpio_out bit
 *   driven by the far end   -> gpio_ext_out bit
 *   nobody driving          -> 1
 *
 * That last line is the pull-up (release/hw/pmods/gpio.spec sets
 * PULLMODE=UP), and it is what makes the open-drain idiom behave here
 * the way it does on a board: park OUT at 0, and DIR becomes the data
 * with float meaning high. An I2C library developed against a model
 * that returned 0 for a floating pin would appear to work and then
 * read nothing but zeros from a real bus.
 *
 * BOTH DRIVING AT ONCE resolves to whatever we are driving, and that
 * is a LIE -- on hardware it is contention, a hot pin and an
 * indeterminate level. The model cannot represent that usefully, so it
 * picks the answer that makes the bug quiet rather than loud, which is
 * the one thing in here worth being suspicious of. Nothing currently
 * sets gpio_ext_dir, so it cannot arise yet.
 *
 * Not modelled: the synchroniser (reads are immediate rather than two
 * cycles late, which no software can tell apart) and the address
 * aliasing gpio.v documents (the window is bounded at 8KB here, so an
 * access above that reads as open bus rather than wrapping). Software
 * that relies on either is wrong anyway.
 */

static uint32_t gpio_pin_state(machine_t *m, int p)
{
	/* pulled high wherever nobody is driving -- see the note above */
	uint8_t undriven = (uint8_t)~(m->gpio_dir[p] | m->gpio_ext_dir[p]);
	return (uint32_t)((m->gpio_out[p] & m->gpio_dir[p])
	                | (m->gpio_ext_out[p] & m->gpio_ext_dir[p]
	                   & (uint8_t)~m->gpio_dir[p])
	                | undriven);
}

static uint32_t gpio_read(machine_t *m, uint32_t addr)
{
	uint32_t off = addr - ZS_GPIO_BASE;

	if (off < 0x1000) {
		switch (off) {
		case 0x00: return m->reg_led;
		case 0x04: return m->reg_leds;
		case 0x08: return 0x5A475049u;                 /* MAGIC "ZGPI" */
		case 0x0c: return 0x47500000u | ZS_GPIO_NPORTS; /* CONFIG "GP" */
		default:   return 0;
		}
	}

	{
		uint32_t rel = off - 0x1000;
		int p = (int)(rel / ZS_GPIO_PORT_SIZE);
		uint32_t r = rel % ZS_GPIO_PORT_SIZE;

		if (p >= ZS_GPIO_NPORTS) return 0;

		switch (r) {
		case 0x00: case 0x14: case 0x18: return m->gpio_dir[p];
		case 0x04: case 0x0c: case 0x10: return m->gpio_out[p];
		case 0x08: return gpio_pin_state(m, p);
		default:   return 0;
		}
	}
}

static void gpio_write(machine_t *m, uint32_t addr, uint32_t val)
{
	uint32_t off = addr - ZS_GPIO_BASE;
	uint8_t v = (uint8_t)(val & 0xff);

	if (off < 0x1000) {
		if (off == 0x00) m->reg_led = val;
		else if (off == 0x04) m->reg_leds = val;
		return;					/* MAGIC/CONFIG are read-only */
	}

	{
		uint32_t rel = off - 0x1000;
		int p = (int)(rel / ZS_GPIO_PORT_SIZE);
		uint32_t r = rel % ZS_GPIO_PORT_SIZE;

		if (p >= ZS_GPIO_NPORTS) return;	/* dropped, as in gpio.v */

		switch (r) {
		case 0x00: m->gpio_dir[p] = v; break;			/* DIR */
		case 0x04: m->gpio_out[p] = v; break;			/* OUT */
		case 0x0c: m->gpio_out[p] |= v; break;			/* OUTSET */
		case 0x10: m->gpio_out[p] &= (uint8_t)~v; break;	/* OUTCLR */
		case 0x14: m->gpio_dir[p] |= v; break;			/* DIRSET */
		case 0x18: m->gpio_dir[p] &= (uint8_t)~v; break;	/* DIRCLR */
		default: break;
		}
	}
}

uint32_t bus_read32(machine_t *m, uint32_t addr) {
	if (addr < ZS_LOWMEM_SIZE) {
		uint32_t v;
		memcpy(&v, &m->lowmem[addr], 4);
		return v;
	}
	if (addr >= ZS_RAM_BASE && addr < ZS_RAM_BASE + m->ram_size) {
		uint32_t v;
		memcpy(&v, &m->ram[addr - ZS_RAM_BASE], 4);
		return v;
	}
	if (addr >= ZS_VRAM_BASE && addr < ZS_VRAM_BASE + ZS_VRAM_WORDS * 4) {
		return m->vram[(addr - ZS_VRAM_BASE) / 4];
	}
	if (addr >= ZS_RASTER_BASE && addr < ZS_RASTER_BASE + 0x40) {
		raster_t *r = &m->raster;
		switch ((addr - ZS_RASTER_BASE) / 4) {
		case 0: return r->x0;
		case 1: return r->y0;
		case 2: return r->x1;
		case 3: return r->y1;
		case 4: return r->color;
		case 5: return 0;         /* start: write-only */
		case 6: return 0;         /* busy: we run synchronously, always done */
		case 7: return r->pixel_count;
		case 8: return r->cur_x;
		case 9: return r->cur_y;
		case 10: return 0;        /* fifo count: always drained synchronously */
		case 11: return r->clip_x0;
		case 12: return r->clip_y0;
		case 13: return r->clip_x1;
		case 14: return r->clip_y1;
		case 15: return r->clip_enable;
		default: return 0;
		}
	}
	if (addr >= ZS_BLIT_BASE && addr < ZS_BLIT_BASE + 0x20) {
		blit_t *b = &m->blit;
		switch ((addr - ZS_BLIT_BASE) / 4) {
		case 0: return (b->clip_enable << 2) | (b->fill << 1);
		case 1: return 0; /* busy: synchronous */
		case 2: return b->dst_x;
		case 3: return b->dst_y;
		case 4: return b->width;
		case 5: return b->height;
		case 6: return b->pattern;
		default: return 0;
		}
	}
	if (addr >= ZS_UART_BASE && addr < ZS_UART_BASE + 0x20) {
		switch (addr - ZS_UART_BASE) {
		case 0x00: { int c = uart_stdin_getc(&m->uart); return c < 0 ? 0 : (uint32_t)c; }
		case 0x14: return uart_stdin_has_byte(&m->uart) ? 0x21 : 0x20; /* LSR: THRE always set */
		default: return 0;
		}
	}
	if (addr >= ZS_USB_BASE && addr < ZS_USB_BASE + 0x10) {
		if (addr - ZS_USB_BASE == 0x0c) return m->usb_cursor;
		return 0;
	}
	if (addr >= ZS_GPIO_BASE && addr < ZS_GPIO_BASE + 0x2000) {
		return gpio_read(m, addr);
	}
	/* MTU, SD card, anything else unmapped: open bus reads as 0 */
	return 0;
}

uint16_t bus_read16(machine_t *m, uint32_t addr) {
	uint32_t w = bus_read32(m, addr & ~3u);
	return (uint16_t)(w >> ((addr & 2) * 8));
}

uint8_t bus_read8(machine_t *m, uint32_t addr) {
	uint32_t w = bus_read32(m, addr & ~3u);
	return (uint8_t)(w >> ((addr & 3) * 8));
}

static void vram_write_word(machine_t *m, uint32_t addr, uint32_t val) {
	m->vram[(addr - ZS_VRAM_BASE) / 4] = val;
}

void bus_write32(machine_t *m, uint32_t addr, uint32_t val) {
	if (addr < ZS_LOWMEM_SIZE) { memcpy(&m->lowmem[addr], &val, 4); return; }
	if (addr >= ZS_RAM_BASE && addr < ZS_RAM_BASE + m->ram_size) {
		memcpy(&m->ram[addr - ZS_RAM_BASE], &val, 4);
		return;
	}
	if (addr >= ZS_VRAM_BASE && addr < ZS_VRAM_BASE + ZS_VRAM_WORDS * 4) {
		vram_write_word(m, addr, val);
		return;
	}
	if (addr >= ZS_RASTER_BASE && addr < ZS_RASTER_BASE + 0x40) {
		raster_t *r = &m->raster;
		switch ((addr - ZS_RASTER_BASE) / 4) {
		case 0: r->x0 = val & 0x1ff; break;
		case 1: r->y0 = val & 0x1ff; break;
		case 2: r->x1 = val & 0x1ff; break;
		case 3: r->y1 = val & 0x1ff; break;
		case 4: r->color = val & 1; break;
		case 5: if (val & 1) raster_run(m); break; /* start */
		case 11: r->clip_x0 = val & 0x1ff; break;
		case 12: r->clip_y0 = val & 0x1ff; break;
		case 13: r->clip_x1 = val & 0x1ff; break;
		case 14: r->clip_y1 = val & 0x1ff; break;
		case 15: r->clip_enable = val & 1; break;
		default: break;
		}
		return;
	}
	if (addr >= ZS_BLIT_BASE && addr < ZS_BLIT_BASE + 0x20) {
		blit_t *b = &m->blit;
		switch ((addr - ZS_BLIT_BASE) / 4) {
		case 0:
			b->fill = (val >> 1) & 1;
			b->clip_enable = (val >> 2) & 1;
			if (val & 1) blit_run(m); /* start */
			break;
		case 2: b->dst_x = val; break;
		case 3: b->dst_y = val; break;
		case 4: b->width = val; break;
		case 5: b->height = val; break;
		case 6: b->pattern = val; break;
		default: break;
		}
		return;
	}
	if (addr >= ZS_UART_BASE && addr < ZS_UART_BASE + 0x20) {
		if (addr - ZS_UART_BASE == 0x00) { putchar((int)(val & 0xff)); fflush(stdout); }
		return;
	}
	if (addr >= ZS_USB_BASE && addr < ZS_USB_BASE + 0x10) return; /* read-only from app's POV */
	if (addr >= ZS_GPIO_BASE && addr < ZS_GPIO_BASE + 0x2000) {
		gpio_write(m, addr, val);
		return;
	}
	/* MTU, SD card, anything else unmapped: open bus write, ignored */
}

void bus_write16(machine_t *m, uint32_t addr, uint16_t val) {
	uint32_t w = bus_read32(m, addr & ~3u);
	unsigned shift = (addr & 2) * 8;
	w = (w & ~(0xffffu << shift)) | ((uint32_t)val << shift);
	bus_write32(m, addr & ~3u, w);
}

void bus_write8(machine_t *m, uint32_t addr, uint8_t val) {
	if (addr == ZS_UART_BASE) { putchar(val); fflush(stdout); return; }
	uint32_t w = bus_read32(m, addr & ~3u);
	unsigned shift = (addr & 3) * 8;
	w = (w & ~(0xffu << shift)) | ((uint32_t)val << shift);
	bus_write32(m, addr & ~3u, w);
}

/* ------------------------------------------------------------------- */

int machine_init(machine_t *m, size_t ram_size) {
	memset(m, 0, sizeof(*m));
	m->ram_size = ram_size ? ram_size : ZS_RAM_DEFAULT_SIZE;
	m->ram = calloc(1, m->ram_size);
	if (!m->ram) return -1;

	m->raster.clip_x1 = 511;
	m->raster.clip_y1 = 511;
	m->blit.clip_enable = 1;

	/* install the syscall gate: reg_kernel (0x0c) points at our trap PC */
	uint32_t trap = ZS_SYSCALL_TRAP_PC;
	memcpy(&m->lowmem[ZS_REG_KERNEL_ADDR], &trap, 4);

	uart_enter_raw();
	return 0;
}

void machine_destroy(machine_t *m) {
	uart_leave_raw();
	free(m->ram);
}

int machine_load_bin(machine_t *m, const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) { perror(path); return -1; }
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz < 0 || (size_t)sz > m->ram_size) {
		fprintf(stderr, "zeitlos-sim: %s (%ld bytes) too large for %zu byte RAM\n",
			path, sz, m->ram_size);
		fclose(f);
		return -1;
	}
	if (fread(m->ram, 1, (size_t)sz, f) != (size_t)sz) {
		fprintf(stderr, "zeitlos-sim: short read on %s\n", path);
		fclose(f);
		return -1;
	}
	fclose(f);

	/* Matches sw/os/kernel.c's k_proc_create(): pc at the app's link
	 * address, sp at the top of its memory region, with the sentinel
	 * return address (0) stored at [sp] so a naturally-returning
	 * main() lands on address 0, which we treat as a clean exit. */
	uint32_t sp = ZS_RAM_BASE + (uint32_t)m->ram_size - 4;
	bus_write32(m, sp, 0);
	cpu_reset(&m->cpu, ZS_RAM_BASE, sp);

	m->running = 1;
	return 0;
}

uint64_t machine_run(machine_t *m, uint64_t max_insns) {
	uint64_t start = m->cpu.insn_count;

	while (m->running && !m->exit_requested) {
		if (max_insns && (m->cpu.insn_count - start) >= max_insns) break;

		if (m->cpu.pc == ZS_SYSCALL_TRAP_PC) {
			do_syscall(m);
			m->cpu.pc = m->cpu.regs[1]; /* return via ra */
			continue;
		}
		if (m->cpu.pc == 0) {
			/* main() returned with no real caller (see machine_load_bin) */
			m->exit_requested = 1;
			m->exit_code = 0;
			break;
		}

		if (cpu_step(&m->cpu, m) != 0) {
			if (m->cpu.trapped == 2) {
				fprintf(stderr, "zeitlos-sim: ECALL/EBREAK at pc=0x%08x, halting\n",
					m->cpu.trap_pc);
			} else {
				fprintf(stderr, "zeitlos-sim: illegal instruction at pc=0x%08x, halting\n",
					m->cpu.trap_pc);
			}
			m->running = 0;
			break;
		}
	}

	m->total_instructions = m->cpu.insn_count;
	return m->cpu.insn_count - start;
}
