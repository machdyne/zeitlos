/*
 * zsys -- the Zeitlos SoC, emulated (zsys.h, docs/sim_parity.md).
 *
 * The CPU follows rtl/cpu/zeitlos32/zeitlos32.v instruction by
 * instruction, including the parts of picorv32's interrupt ABI that
 * look like bugs (see the header there). The bus follows rtl/sysctl.v's
 * decode. Each device is its registers.
 *
 * -- time --
 *
 * Each retired instruction costs `cpi` clocks of the board's 48MHz
 * sys_clk (2 by default; zeitlos32 takes 3-5 with its caches, so the
 * emulated board is a little quicker than the real one, never slower,
 * and rdcycle still outpaces rdinstret as t_csr.S expects). The tick (IRQ 3), the IRQ timer and rdcycle
 * all count these, so they agree with each other as they do on the
 * board. A front end that wants wall-clock pace runs zsys_run() in
 * slices and sleeps between them.
 *
 * waitirq with nothing pending skips straight to the next event -- the
 * next tick or the timer -- instead of spinning through the cycles in
 * between, which is what makes an idle emulated machine idle on the
 * host too.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "zsys.h"

#define TB_MEM_SIZE    (64u * 1024)
#define SOC_BRAM_SIZE  (8u * 1024)		// BRAM_WORDS 2048 (sw/bios/Makefile)
#define SOC_VRAM_SIZE  (128u * 1024)		// the VRAM port's address bits [16:2]
#define TICK_CLOCKS    65536u			// rtl/sysctl.v: rtc_ctr, 16 bits

#define LATCHED_SOC    0xfffffd6fu		// rtl/sysctl.v: LATCHED_IRQ
#define LATCHED_TB     0xffffffefu		// tb_zeitlos32.v

#define IRQ_TIMER  (1u << 0)			// the CPU's IRQ timer
#define IRQ_ILLEGAL (1u << 1)
#define IRQ_MISALIGN (1u << 2)
#define IRQ_TICK   (1u << 3)
#define IRQ_UART   (1u << 4)

// -- setup --

bool zsys_init(zsys_t *s, zsys_kind_t kind, uint32_t ram_mb) {
	memset(s, 0, sizeof(*s));
	s->kind = kind;
	s->cpi = 2;
	s->sd.fd = -1;
	s->flash_fd = -1;
	if (kind == ZSYS_TB) {
		s->bram_size = TB_MEM_SIZE;
		s->latched = LATCHED_TB;
	} else {
		s->bram_size = SOC_BRAM_SIZE;
		s->ram_size = ram_mb * 1024u * 1024u;
		s->vram_size = SOC_VRAM_SIZE;
		s->latched = LATCHED_SOC;
		s->mem_mb = ram_mb;
	}
	s->bram = calloc(1, s->bram_size);
	if (s->ram_size) s->ram = calloc(1, s->ram_size);
	if (s->vram_size) s->vram = calloc(1, s->vram_size);
	return s->bram && (!s->ram_size || s->ram) && (!s->vram_size || s->vram);
}

void zsys_free(zsys_t *s) {
	free(s->bios_copy);
	free(s->sd.img);
	free(s->sd.written);
	if (s->sd.fd >= 0) close(s->sd.fd);
	if (s->flash_fd >= 0) close(s->flash_fd);
	free(s->bram);
	free(s->ram);
	free(s->vram);
	free(s->flash);
	memset(s, 0, sizeof(*s));
}

bool zsys_load_bram(zsys_t *s, const uint8_t *data, uint32_t len) {
	if (len > s->bram_size) return false;
	memcpy(s->bram, data, len);
	// kept: a reconfiguration (socctl's RECONFIG) loads the bitstream,
	// and with it the BIOS in BRAM, again
	if (s->kind == ZSYS_SOC && data != s->bios_copy) {
		free(s->bios_copy);
		s->bios_copy = malloc(len ? len : 1);
		if (s->bios_copy) memcpy(s->bios_copy, data, len);
		s->bios_len = len;
	}
	return true;
}

bool zsys_load_flash(zsys_t *s, const uint8_t *data, uint32_t len,
		const uint8_t **bios, uint32_t *bios_len) {
	free(s->flash);
	s->flash = malloc(len);
	if (!s->flash) return false;
	memcpy(s->flash, data, len);
	s->flash_size = len;
	if (bios) *bios = NULL;
	if (bios_len) *bios_len = 0;
	if (len >= 12 && !memcmp(data, ZSYS_IMG_MAGIC, 8)) {
		uint32_t n = (uint32_t)data[8] | (uint32_t)data[9] << 8 | (uint32_t)data[10] << 16 |
			(uint32_t)data[11] << 24;
		if (n && 12 + n <= len) {
			if (bios) *bios = s->flash + 12;
			if (bios_len) *bios_len = n;
		}
	}
	return true;
}

void zsys_reset(zsys_t *s) {
	memset(s->x, 0, sizeof(s->x));
	memset(s->q, 0, sizeof(s->q));
	s->pc = 0;
	s->x[2] = s->kind == ZSYS_TB ? 0x00010000u : s->bram_size;	// STACKADDR
	s->irq_pending = 0;
	s->irq_mask = 0xffffffffu;
	s->irq_active = s->irq_delay = false;
	s->in_waitirq = false;
	s->timer = 0;
	s->cycles = s->instret = 0;
	s->next_tick = TICK_CLOCKS;
	s->stop = ZSYS_RUNNING;
	s->mtu = 0;
	s->uart_ier = s->uart_lcr = s->uart_mcr = s->uart_fcr = 0;
	s->uart_thre_int = false;
	s->spisd_ctrl = 59u << 8;			// DEFAULT_DIV
	s->spisd_data = 0;
	s->reboot = false;
	if (s->kind == ZSYS_SOC) zdev_reset(s);
}

// -- UART0: a 16550 (rtl/uart.v) --

static bool uart_rx_ready(const zsys_t *s) { return s->rx_head != s->rx_tail; }

bool zsys_uart_in(zsys_t *s, uint8_t c) {
	uint32_t next = (s->rx_head + 1) % ZSYS_UART_FIFO;
	if (next == s->rx_tail) return false;
	s->rx[s->rx_head] = c;
	s->rx_head = next;
	return true;
}

// The interrupt identification, by priority: received data, then THRE.
static uint8_t uart_iir(const zsys_t *s) {
	uint8_t fifo = (s->uart_fcr & 1) ? 0xc0 : 0x00;
	if ((s->uart_ier & 1) && uart_rx_ready(s)) return fifo | 0x04;
	if ((s->uart_ier & 2) && s->uart_thre_int) return fifo | 0x02;
	return fifo | 0x01;
}

static uint32_t uart_read(zsys_t *s, uint32_t reg) {
	bool dlab = (s->uart_lcr & 0x80) != 0;
	switch (reg) {
	case 0:
		if (dlab) return s->uart_dll;
		if (!uart_rx_ready(s)) return 0;
		{ uint8_t c = s->rx[s->rx_tail]; s->rx_tail = (s->rx_tail + 1) % ZSYS_UART_FIFO; return c; }
	case 1: return dlab ? s->uart_dlh : s->uart_ier;
	case 2: {
		uint8_t iir = uart_iir(s);
		if ((iir & 0x0f) == 0x02) s->uart_thre_int = false;	// reading IIR acknowledges THRE
		return iir;
	}
	case 3: return s->uart_lcr;
	case 4: return s->uart_mcr;
	case 5: return 0x60 | (uart_rx_ready(s) ? 1 : 0);	// THRE and TEMT: sent at once
	case 6: return 0xb0;				// CTS, DSR, DCD
	case 7: return s->uart_scr;
	}
	return 0;
}

static void uart_write(zsys_t *s, uint32_t reg, uint8_t v) {
	bool dlab = (s->uart_lcr & 0x80) != 0;
	switch (reg) {
	case 0:
		if (dlab) { s->uart_dll = v; return; }
		if (s->uart_out) s->uart_out(s, v);
		s->uart_thre_int = true;		// written, and at once empty again
		return;
	case 1:
		if (dlab) { s->uart_dlh = v; return; }
		// THRE enabled with the holding register empty: an interrupt now
		if ((v & 2) && !(s->uart_ier & 2)) s->uart_thre_int = true;
		s->uart_ier = v & 0x0f;
		return;
	case 2:
		s->uart_fcr = v;
		if (v & 2) s->rx_tail = s->rx_head;	// FCR bit 1: clear the receive FIFO
		return;
	case 3: s->uart_lcr = v; return;
	case 4: s->uart_mcr = v; return;
	case 7: s->uart_scr = v; return;
	}
}

static bool uart_irq(const zsys_t *s) { return (uart_iir(s) & 1) == 0; }

// -- the bus (rtl/sysctl.v) --

static inline uint32_t phys(const zsys_t *s, uint32_t a) {
	if (s->mtu && (a & 0xf0000000u) == 0x80000000u) return s->mtu + (a & 0x0fffffffu);
	return a;
}

// Memory that is memory: a pointer to `n` bytes at physical `pa`, or
// NULL for a device (or nothing). Each region mirrors through its
// select, as the RTL's decode does.
static inline uint8_t *mem(zsys_t *s, uint32_t pa, bool write) {
	switch (pa >> 28) {
	case 0x0: return s->bram + (pa & (s->bram_size - 1));
	case 0x4: return s->ram_size ? s->ram + (pa & (s->ram_size - 1)) : NULL;
	case 0x2: return s->vram_size ? s->vram + (pa & (s->vram_size - 1)) : NULL;
	case 0x3: return s->kind == ZSYS_SOC ? s->glyph + (pa & (ZSYS_GLYPH_SIZE - 1)) : NULL;
	case 0x1:
		if (write || !s->flash) return NULL;
		{ uint32_t o = pa & 0x0fffffffu; return o < s->flash_size ? s->flash + o : NULL; }
	}
	return NULL;
}

static void unmapped(zsys_t *s, uint32_t pa, bool write) {
	uint32_t bit = 1u << (pa >> 28);
	if (s->unmapped_said & bit) return;
	s->unmapped_said |= bit;
	fprintf(stderr, "zsys: %s %08lx: nothing there (said once per region) at pc %08lx\n",
		write ? "write" : "read", (unsigned long)pa, (unsigned long)s->pc);
}

static uint32_t io_read(zsys_t *s, uint32_t pa) {

	switch (pa >> 28) {

	case 0x1:
		if (s->kind == ZSYS_SOC && (pa & 0x0ffffe00u) == 0x0f000000u) {	// rtl/spiflash.v's registers
			bool ok; uint32_t v = zdev_read(s, pa, &ok); if (ok) return v;
		}
		return 0xffffffffu;			// past the end of the flash: erased

	case 0x7:
		if (s->t2_on && (pa & 0xff000000u) == 0x7c000000u) return zt2_read(s, (pa & 0x3f) >> 2);	// T2
		if ((pa & 0xf0000700u) == 0x70000000u) {	// the CSRs (rtl/csrs.v)
			switch ((pa & 0xff) >> 2) {
			case 0: return 0x5a454954u;		// "ZEIT"
			case 1: return s->mem_mb;
			case 2: return s->features;
			case 3: return 0x5a460000u | (s->features2 & 0xffff);
			case 4: return 0x5a330000u | (s->features3 & 0xffff);
			case 63: return s->flash_base;
			}
			return 0;
		}
		if (s->kind == ZSYS_SOC) {			// socctl, the RTC, ...
			bool ok;
			uint32_t v = zdev_read(s, pa, &ok);
			if (ok) return v;
			return 0;				// csrs.v acks the rest of the window
		}
		break;

	case 0x9:
		return s->mtu;

	case 0xa: case 0xb: case 0xc: case 0xd:
		// the rasterizer, the SD card's SPI controller, the USB HID ports, the blitter
		if (s->kind == ZSYS_TB) break;
		{ bool ok; uint32_t v = zdev_read(s, pa, &ok); if (ok) return v; }
		break;

	case 0xe:
		if (s->kind == ZSYS_TB) return 0;
		if ((pa & 0xff) == 0x00) return s->led;
		if ((pa & 0xff) == 0x04) return s->leds;
		return 0;

	case 0xf:
		if ((pa & 0xf0000700u) == 0xf0000000u) return uart_read(s, (pa & 0x1f) >> 2);
		if ((pa & 0xf0000700u) == 0xf0000400u && (s->features2 & (1u << 11))) return zsys_vmouse_read(s);
		break;

	}

	unmapped(s, pa, false);
	return 0;

}

// `v` is in its byte lanes, `sel` says which (wishbone's sel).
static void io_write(zsys_t *s, uint32_t pa, uint32_t v, uint32_t sel) {

	switch (pa >> 28) {

	case 0x1:
		if (s->kind == ZSYS_SOC && (pa & 0x0ffffe00u) == 0x0f000000u) zdev_write(s, pa & ~3u, v, sel);
		return;					// the window itself: acked and ignored

	case 0x7:
		if (s->t2_on && (pa & 0xff000000u) == 0x7c000000u) { zt2_write(s, (pa & 0x3f) >> 2, v); return; }
		if (s->kind == ZSYS_SOC) zdev_write(s, pa & ~3u, v, sel);	// socctl, the RTC
		return;					// the CSRs themselves: read-only

	case 0x9:
		s->mtu = v;
		return;

	case 0xa: case 0xb: case 0xc: case 0xd:
		if (s->kind == ZSYS_TB) break;
		if (zdev_write(s, pa & ~3u, v, sel)) return;
		break;

	case 0xe:
		if (s->kind == ZSYS_TB) {
			v >>= 8 * (pa & 3);
			switch (pa & 0xfc) {
			case 0x00: s->tb_result = v; s->stop = ZSYS_TB_DONE; return;
			// one cycle: only a latched line remembers it, an
			// unlatched one (IRQ 4) is gone by the next cycle
			case 0x10: s->irq_pending |= v & s->latched; return;
			case 0x14: s->irq_level = v; return;
			}
			return;
		}
		if ((pa & 0xfc) == 0x00) s->led = v;
		if ((pa & 0xfc) == 0x04) s->leds = v;
		return;

	case 0xf:
		if ((pa & 0xf0000700u) == 0xf0000000u) { uart_write(s, (pa & 0x1f) >> 2, (uint8_t)(v >> 8 * (pa & 3))); return; }
		if ((pa & 0xf0000700u) == 0xf0000400u && (s->features2 & (1u << 11))) { zsys_vmouse_write(s, v); return; }
		break;

	}

	unmapped(s, pa, true);

}

static inline uint32_t load(zsys_t *s, uint32_t va, int size) {
	uint32_t pa = phys(s, va);
	uint8_t *p = mem(s, pa, false);
	if (p) {
		if (size == 4) { uint32_t v; memcpy(&v, p, 4); return v; }
		if (size == 2) { uint16_t v; memcpy(&v, p, 2); return v; }
		return *p;
	}
	uint32_t v = io_read(s, pa & ~3u);
	return v >> (8 * (pa & 3));
}

static inline void store(zsys_t *s, uint32_t va, uint32_t v, int size) {
	uint32_t pa = phys(s, va);
	uint8_t *p = mem(s, pa, true);
	if (p) {
		if (size == 4) memcpy(p, &v, 4);
		else if (size == 2) { uint16_t h = (uint16_t)v; memcpy(p, &h, 2); }
		else *p = (uint8_t)v;
		if ((pa >> 28) == 0x2) zsys_vram_dirty(s, pa & (s->vram_size - 1));
		return;
	}
	uint32_t lane = pa & 3;
	uint32_t sel = (size == 4 ? 0xfu : size == 2 ? 0x3u : 0x1u) << lane;
	io_write(s, pa, v << 8 * lane, sel);
}

// -- the CPU --

#define RD   ((insn >> 7) & 31)
#define RS1  ((insn >> 15) & 31)
#define RS2  ((insn >> 20) & 31)
#define F3   ((insn >> 12) & 7)
#define F7   (insn >> 25)

static inline int32_t imm_i(uint32_t insn) { return (int32_t)insn >> 20; }
static inline int32_t imm_s(uint32_t insn) { return ((int32_t)insn >> 25 << 5) | ((insn >> 7) & 31); }
static inline int32_t imm_b(uint32_t insn) {
	return ((int32_t)insn >> 31 << 12) | ((insn & 0x80) << 4) | ((insn >> 20) & 0x7e0) | ((insn >> 7) & 0x1e);
}
static inline int32_t imm_j(uint32_t insn) {
	return ((int32_t)insn >> 31 << 20) | (insn & 0xff000) | ((insn >> 9) & 0x800) | ((insn >> 20) & 0x7fe);
}

// A fault on line `bit` (1 illegal/ecall/ebreak, 2 misaligned): the
// instruction is retired without running and the line pends, or -- the
// line masked, or already in a handler -- the core halts (zeitlos32.v,
// "fault notes").
static inline bool fault(zsys_t *s, uint32_t bit, const char *why) {
	if (!(s->irq_mask & bit) && !s->irq_active) { s->irq_pending |= bit; return true; }
	s->stop = ZSYS_TRAPPED;
	s->trap_pc = s->pc;
	s->trap_why = why;
	return false;
}

// Time passes: the tick and the IRQ timer.
static inline void clocks(zsys_t *s, uint32_t n) {
	s->cycles += n;
	zsys_audio_clock(s, n);
	if (s->timer) {
		if (s->timer <= n) { s->timer = 0; s->irq_pending |= IRQ_TIMER; }
		else s->timer -= n;
	}
	if (s->kind == ZSYS_SOC && s->cycles >= s->next_tick) {
		s->irq_pending |= IRQ_TICK;
		if (s->on_tick) s->on_tick(s);
		while (s->next_tick <= s->cycles) s->next_tick += TICK_CLOCKS;
	}
}

// The incoming lines, folded in as at the end of every cycle on the
// board: latched bits stay until delivered, the others follow the level.
static inline void fold(zsys_t *s) {
	uint32_t level = s->irq_level;
	if (s->kind == ZSYS_SOC && uart_irq(s)) level |= IRQ_UART;
	if (s->audio_on && zdev_audio_irq(s)) level |= 1u << 7;	// IRQ 7: the FIFO below its watermark
	s->irq_pending = (s->irq_pending & s->latched) | level;
}

int zsys_run(zsys_t *s, uint64_t budget) {

	uint64_t end = s->cycles + budget;

	s->stop = ZSYS_RUNNING;

	while (s->stop == ZSYS_RUNNING && s->cycles < end) {

		// socctl's RECONFIG: the FPGA loads its bitstream again -- the
		// BIOS back in BRAM, every device at power-on, the CPU at 0
		if (s->reboot) {
			uint64_t c = s->cycles;
			if (s->bios_copy) zsys_load_bram(s, s->bios_copy, s->bios_len);
			zsys_reset(s);
			s->cycles = s->rtc_cycles = c;	// time goes on; the RTC starts again
			s->next_tick = c + TICK_CLOCKS;
			end = c + budget;
			s->reboots++;
			continue;
		}

		fold(s);

		// -- stalled in waitirq (zeitlos32's ST_WAITIRQ): no interrupt
		// is taken from here; as soon as anything is pending, waitirq
		// hands the set to software and retires --
		if (s->in_waitirq) {
			if (s->irq_pending) {
				uint32_t rd = (load(s, s->pc, 4) >> 7) & 31;
				if (rd) s->x[rd] = s->irq_pending;
				s->in_waitirq = false;
				s->pc += 4;
				s->instret++;
				clocks(s, s->cpi);
				continue;
			}
			// skip to the next thing that could end it
			uint64_t wake = s->kind == ZSYS_SOC ? s->next_tick : end;
			if (s->timer && s->cycles + s->timer < wake) wake = s->cycles + s->timer;
			if (wake > end) wake = end;
			if (s->kind == ZSYS_TB && !s->timer && !s->irq_level) { s->stop = ZSYS_IDLE; break; }
			if (s->kind == ZSYS_SOC && uart_rx_ready(s)) wake = s->cycles + 1;
			// the audio interrupt can come up at a frame: wake there
			if (s->audio_on && (s->audio.ctrl & 3) == 3) {
				uint64_t f = s->cycles + zdev_audio_next_frame(s);
				if (f < wake) wake = f;
			}
			clocks(s, (uint32_t)(wake > s->cycles ? wake - s->cycles : 1));
			continue;
		}

		// -- an instruction boundary: take an interrupt? --
		uint32_t delivered = s->irq_pending & ~s->irq_mask;
		if (delivered && !s->irq_active && !s->irq_delay) {
			s->q[0] = s->pc;
			s->q[1] = delivered;
			for (uint32_t d = delivered, i = 0; d; d >>= 1, i++) if (d & 1) s->irq_count[i]++;
			s->irq_pending &= s->irq_mask;
			s->irq_active = true;
			s->pc = 0x10;				// PROGADDR_IRQ
			clocks(s, 1);
			continue;
		}
		s->irq_delay = s->irq_active;

		if (s->pc & 3) {
			fault(s, IRQ_MISALIGN, "a misaligned pc");
			clocks(s, 1);
			continue;
		}

		uint32_t insn = load(s, s->pc, 4);
		uint32_t next = s->pc + 4;
		uint32_t a = s->x[RS1], b = s->x[RS2], r = 0;
		bool wr = false;

		switch (insn & 0x7f) {

		case 0x37: r = insn & 0xfffff000u; wr = true; break;			// lui
		case 0x17: r = s->pc + (insn & 0xfffff000u); wr = true; break;		// auipc
		case 0x6f: r = next; next = s->pc + imm_j(insn); wr = true; break;	// jal
		case 0x67:								// jalr
			if (F3) { fault(s, IRQ_ILLEGAL, "illegal"); break; }
			r = next; next = (a + imm_i(insn)) & ~1u; wr = true; break;

		case 0x63: {								// branches
			bool t;
			switch (F3) {
			case 0: t = a == b; break;
			case 1: t = a != b; break;
			case 4: t = (int32_t)a < (int32_t)b; break;
			case 5: t = (int32_t)a >= (int32_t)b; break;
			case 6: t = a < b; break;
			case 7: t = a >= b; break;
			default: fault(s, IRQ_ILLEGAL, "illegal"); t = false; break;
			}
			if (t) next = s->pc + imm_b(insn);
			break;
		}

		case 0x03: {								// loads
			uint32_t ea = a + imm_i(insn);
			switch (F3) {
			case 0: r = (uint32_t)(int32_t)(int8_t)load(s, ea, 1); wr = true; break;
			case 4: r = load(s, ea, 1) & 0xff; wr = true; break;
			case 1: case 5:
				if (ea & 1) { fault(s, IRQ_MISALIGN, "a misaligned load"); break; }
				r = load(s, ea, 2) & 0xffff;
				if (F3 == 1) r = (uint32_t)(int32_t)(int16_t)r;
				wr = true; break;
			case 2:
				if (ea & 3) { fault(s, IRQ_MISALIGN, "a misaligned load"); break; }
				r = load(s, ea, 4); wr = true; break;
			default: fault(s, IRQ_ILLEGAL, "illegal"); break;
			}
			break;
		}

		case 0x23: {								// stores
			uint32_t ea = a + imm_s(insn);
			switch (F3) {
			case 0: store(s, ea, b, 1); break;
			case 1: if (ea & 1) fault(s, IRQ_MISALIGN, "a misaligned store"); else store(s, ea, b, 2); break;
			case 2: if (ea & 3) fault(s, IRQ_MISALIGN, "a misaligned store"); else store(s, ea, b, 4); break;
			default: fault(s, IRQ_ILLEGAL, "illegal"); break;
			}
			break;
		}

		case 0x13: {								// ALU, immediate
			int32_t im = imm_i(insn);
			uint32_t sh = (insn >> 20) & 31;
			wr = true;
			switch (F3) {
			case 0: r = a + im; break;
			case 2: r = (int32_t)a < im; break;
			case 3: r = a < (uint32_t)im; break;
			case 4: r = a ^ im; break;
			case 6: r = a | im; break;
			case 7: r = a & im; break;
			case 1: if (F7) { wr = false; fault(s, IRQ_ILLEGAL, "illegal"); } else r = a << sh; break;
			case 5:
				if (F7 == 0) r = a >> sh;
				else if (F7 == 0x20) r = (uint32_t)((int32_t)a >> sh);
				else { wr = false; fault(s, IRQ_ILLEGAL, "illegal"); }
				break;
			}
			break;
		}

		case 0x33:								// ALU, registers; M
			wr = true;
			if (F7 == 1) {
				switch (F3) {
				case 0: r = a * b; break;
				case 1: r = (uint32_t)(((int64_t)(int32_t)a * (int64_t)(int32_t)b) >> 32); break;
				case 2: r = (uint32_t)(((int64_t)(int32_t)a * (int64_t)(uint64_t)b) >> 32); break;
				case 3: r = (uint32_t)(((uint64_t)a * (uint64_t)b) >> 32); break;
				case 4:
					if (!b) r = 0xffffffffu;
					else if (a == 0x80000000u && b == 0xffffffffu) r = a;
					else r = (uint32_t)((int32_t)a / (int32_t)b);
					break;
				case 5: r = b ? a / b : 0xffffffffu; break;
				case 6:
					if (!b) r = a;
					else if (a == 0x80000000u && b == 0xffffffffu) r = 0;
					else r = (uint32_t)((int32_t)a % (int32_t)b);
					break;
				case 7: r = b ? a % b : a; break;
				}
			} else if (F7 == 0 || F7 == 0x20) {
				switch (F3) {
				case 0: r = F7 ? a - b : a + b; break;
				case 1: r = a << (b & 31); break;
				case 2: r = (int32_t)a < (int32_t)b; break;
				case 3: r = a < b; break;
				case 4: r = a ^ b; break;
				case 5: r = F7 ? (uint32_t)((int32_t)a >> (b & 31)) : a >> (b & 31); break;
				case 6: r = a | b; break;
				case 7: r = a & b; break;
				}
				if (F7 == 0x20 && F3 != 0 && F3 != 5) { wr = false; fault(s, IRQ_ILLEGAL, "illegal"); }
			} else { wr = false; fault(s, IRQ_ILLEGAL, "illegal"); }
			break;

		case 0x0f:								// fence: a nop
			break;

		case 0x73:								// system
			if (F3 == 2 && RS1 == 0) {					// csrrs rd, csr, x0: the counters
				uint32_t csr = insn >> 20;
				wr = true;
				switch (csr) {
				case 0xc00: case 0xc01: r = (uint32_t)s->cycles; break;
				case 0xc80: case 0xc81: r = (uint32_t)(s->cycles >> 32); break;
				case 0xc02: r = (uint32_t)s->instret; break;
				case 0xc82: r = (uint32_t)(s->instret >> 32); break;
				default: wr = false; fault(s, IRQ_ILLEGAL, "an unknown csr"); break;
				}
			} else fault(s, IRQ_ILLEGAL, insn == 0x00100073u ? "ebreak" : insn == 0x00000073u ? "ecall" : "illegal");
			break;

		case 0x0b:								// picorv32 custom-0
			switch (F7) {
			case 0: r = s->q[RS1 & 3]; wr = true; break;			// getq
			case 1: s->q[RD & 3] = a; break;				// setq
			case 2: next = s->q[0]; s->irq_active = false; break;		// retirq
			case 3: r = s->irq_mask; s->irq_mask = a; wr = true; break;	// maskirq
			case 4:								// waitirq
				s->waitirqs++;
				if (s->irq_pending) { r = s->irq_pending; wr = true; break; }
				s->in_waitirq = true;					// stall: see the top of the loop
				clocks(s, 1);
				continue;
			case 5: r = s->timer; s->timer = a; wr = true; s->timers++; break;	// timer (zeitlos32p)
			default: fault(s, IRQ_ILLEGAL, "illegal"); break;
			}
			break;

		default:
			fault(s, IRQ_ILLEGAL, "illegal");
			break;

		}

		if (s->stop == ZSYS_TRAPPED) break;
		if (wr && RD) s->x[RD] = r;
		s->pc = next;
		s->instret++;
		clocks(s, s->cpi);

	}

	if (s->stop == ZSYS_RUNNING) s->stop = ZSYS_LIMIT;
	return s->stop;

}
