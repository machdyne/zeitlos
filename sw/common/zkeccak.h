#ifndef ZKECCAK_H
#define ZKECCAK_H
/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Keccak-f[1600]: on the Keccak block (rtl/keccak.v) when the bitstream
 * has it and nobody else holds it, in software otherwise -- the same
 * result either way. docs/keccak_hw.md. zfips202.c builds SHA-3 and
 * SHAKE on it.
 */
#include <stdint.h>

// The permutation, in place. state[i] is lane i = x + 5y (FIPS 202).
void zkeccak_f1600(uint64_t state[25]);

// Which path the last call took: 1 the block, 0 software. For tests
// and cryptobench.
extern int zkeccak_last_hw;
// Tests and cryptobench: force software (1), or allow the block (0).
extern int zkeccak_no_hw;

#endif
