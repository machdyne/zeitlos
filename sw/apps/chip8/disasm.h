#ifndef CHIP8_DISASM_H
#define CHIP8_DISASM_H

/*
 * chip8 -- disassembler.
 *
 * Octo syntax, because that is what the entire modern CHIP-8 toolchain
 * reads and writes. The classic `LD Vx, byte` mnemonics are older and
 * nobody writes them any more.
 *
 * Not quite round-trippable: Octo spells a call as a bare label, and a
 * ROM image has no labels, so `2NNN` comes out as `call 0xNNN` which
 * Octo would not accept. Everything else is real Octo.
 *
 * Pure, like core.c and render.c -- takes bytes, writes text, includes
 * nothing from sw/common.
 *
 * Guest memory is passed rather than a c8_t so that this can also
 * disassemble a ROM image that is not loaded into a machine, which is
 * what a future `chip8 -d` would want.
 */

#include <stdint.h>

/* Longest line this ever writes, including the NUL. */
#define C8_DISASM_MAX 40

/* Disassemble the instruction at `pc`.
 *
 * `ram` is guest memory and `mask` is the address mask that applies to
 * it (0x0FFF or 0xFFFF), so a read that runs off the end wraps exactly
 * as the interpreter's would rather than reading past the array.
 *
 * Writes the operands only -- no address, no opcode bytes. The caller
 * formats those, because a debugger pane and a listing want them
 * differently and neither wants to strip the other's.
 *
 * Returns the instruction's LENGTH IN BYTES, which is 2 for everything
 * except XO-CHIP's `F000 NNNN` long load. A caller stepping through
 * memory must use it: advancing by a fixed 2 through a long load puts
 * every subsequent line out of phase, and the result looks like the
 * ROM is full of garbage rather than like the disassembler is wrong.
 */
int c8_disasm(const uint8_t *ram, uint16_t pc, uint16_t mask,
	char *out, int outlen);

#endif
