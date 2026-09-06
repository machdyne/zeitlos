/*
 * chip8 -- disassembler. See disasm.h.
 */

#include <stdio.h>

#include "disasm.h"

static uint8_t rd(const uint8_t *ram, uint16_t a, uint16_t mask) {
	return ram[a & mask];
}

int c8_disasm(const uint8_t *ram, uint16_t pc, uint16_t mask,
	char *out, int outlen) {

	uint16_t op = (uint16_t)((rd(ram, pc, mask) << 8) |
	                          rd(ram, (uint16_t)(pc + 1), mask));

	unsigned nnn = op & 0x0FFF;
	unsigned x   = (op >> 8) & 0x0F;
	unsigned y   = (op >> 4) & 0x0F;
	unsigned kk  = op & 0xFF;
	unsigned n   = op & 0x0F;

	int len = 2;

	if (outlen < 8) { if (outlen > 0) out[0] = '\0'; return len; }

	switch (op >> 12) {

	case 0x0:
		if (op == 0x00E0)                 snprintf(out, outlen, "clear");
		else if (op == 0x00EE)            snprintf(out, outlen, "return");
		else if ((op & 0xFFF0) == 0x00C0) snprintf(out, outlen, "scroll-down %u", n);
		else if ((op & 0xFFF0) == 0x00D0) snprintf(out, outlen, "scroll-up %u", n);
		else if (op == 0x00FB)            snprintf(out, outlen, "scroll-right");
		else if (op == 0x00FC)            snprintf(out, outlen, "scroll-left");
		else if (op == 0x00FD)            snprintf(out, outlen, "exit");
		else if (op == 0x00FE)            snprintf(out, outlen, "lores");
		else if (op == 0x00FF)            snprintf(out, outlen, "hires");
		else                              snprintf(out, outlen, "; machine 0x%03X", nnn);
		break;

	case 0x1: snprintf(out, outlen, "jump 0x%03X", nnn); break;
	/* Octo spells a call as a bare label, which a disassembler has
	 * no way to produce -- there are no labels in a ROM image. `call`
	 * is not Octo, and is the one place this output is not
	 * round-trippable; it is far more readable than a bare address
	 * would be, which is what a debugger pane is for. */
	case 0x2: snprintf(out, outlen, "call 0x%03X", nnn); break;

	case 0x3: snprintf(out, outlen, "if v%X != 0x%02X then", x, kk); break;
	case 0x4: snprintf(out, outlen, "if v%X == 0x%02X then", x, kk); break;

	case 0x5:
		if (n == 0)      snprintf(out, outlen, "if v%X != v%X then", x, y);
		else if (n == 2) snprintf(out, outlen, "save v%X - v%X", x, y);
		else if (n == 3) snprintf(out, outlen, "load v%X - v%X", x, y);
		else             snprintf(out, outlen, "; bad 0x%04X", op);
		break;

	case 0x6: snprintf(out, outlen, "v%X := 0x%02X", x, kk); break;
	case 0x7: snprintf(out, outlen, "v%X += 0x%02X", x, kk); break;

	case 0x8:
		switch (n) {
		case 0x0: snprintf(out, outlen, "v%X := v%X", x, y); break;
		case 0x1: snprintf(out, outlen, "v%X |= v%X", x, y); break;
		case 0x2: snprintf(out, outlen, "v%X &= v%X", x, y); break;
		case 0x3: snprintf(out, outlen, "v%X ^= v%X", x, y); break;
		case 0x4: snprintf(out, outlen, "v%X += v%X", x, y); break;
		case 0x5: snprintf(out, outlen, "v%X -= v%X", x, y); break;
		case 0x6: snprintf(out, outlen, "v%X >>= v%X", x, y); break;
		case 0x7: snprintf(out, outlen, "v%X =- v%X", x, y); break;
		case 0xE: snprintf(out, outlen, "v%X <<= v%X", x, y); break;
		default:  snprintf(out, outlen, "; bad 0x%04X", op); break;
		}
		break;

	case 0x9:
		if (n == 0) snprintf(out, outlen, "if v%X == v%X then", x, y);
		else        snprintf(out, outlen, "; bad 0x%04X", op);
		break;

	case 0xA: snprintf(out, outlen, "i := 0x%03X", nnn); break;
	case 0xB: snprintf(out, outlen, "jump0 0x%03X", nnn); break;
	case 0xC: snprintf(out, outlen, "v%X := random 0x%02X", x, kk); break;
	case 0xD: snprintf(out, outlen, "sprite v%X v%X %u", x, y, n); break;

	case 0xE:
		if (kk == 0x9E)      snprintf(out, outlen, "if v%X -key then", x);
		else if (kk == 0xA1) snprintf(out, outlen, "if v%X key then", x);
		else                 snprintf(out, outlen, "; bad 0x%04X", op);
		break;

	case 0xF:
		/* The one four-byte instruction. Reported as length 4 so a
		 * caller walking memory does not step into the middle of the
		 * address literal -- which produces a listing that looks like
		 * a corrupt ROM rather than like a disassembler bug. */
		if (op == 0xF000) {
			unsigned a = (unsigned)((rd(ram, (uint16_t)(pc + 2), mask) << 8) |
			                         rd(ram, (uint16_t)(pc + 3), mask));
			snprintf(out, outlen, "i := long 0x%04X", a);
			len = 4;
			break;
		}
		if (op == 0xF002) { snprintf(out, outlen, "audio"); break; }
		if ((op & 0xF0FF) == 0xF001) {
			snprintf(out, outlen, "plane %u", x);
			break;
		}
		switch (kk) {
		case 0x07: snprintf(out, outlen, "v%X := delay", x); break;
		case 0x0A: snprintf(out, outlen, "v%X := key", x); break;
		case 0x15: snprintf(out, outlen, "delay := v%X", x); break;
		case 0x18: snprintf(out, outlen, "buzzer := v%X", x); break;
		case 0x1E: snprintf(out, outlen, "i += v%X", x); break;
		case 0x29: snprintf(out, outlen, "i := hex v%X", x); break;
		case 0x30: snprintf(out, outlen, "i := bighex v%X", x); break;
		case 0x33: snprintf(out, outlen, "bcd v%X", x); break;
		case 0x3A: snprintf(out, outlen, "pitch := v%X", x); break;
		case 0x55: snprintf(out, outlen, "save v%X", x); break;
		case 0x65: snprintf(out, outlen, "load v%X", x); break;
		case 0x75: snprintf(out, outlen, "saveflags v%X", x); break;
		case 0x85: snprintf(out, outlen, "loadflags v%X", x); break;
		default:   snprintf(out, outlen, "; bad 0x%04X", op); break;
		}
		break;

	default:
		snprintf(out, outlen, "; bad 0x%04X", op);
		break;

	}

	return len;

}
