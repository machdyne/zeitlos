/*
 *  PicoSoC - A simple example SoC using PicoRV32
 *
 *  Copyright (C) 2017  Claire Xenia Wolf <claire@yosyshq.com>
 *  Copyright (c) 2021  Lone Dynamics Corporation
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include <stdio.h>
#include <stdint.h>

int putchar(int c);
void print(const char *p);

uint32_t xorshift32(uint32_t *state);

void print_hex(uint32_t v, int digits)
{
	for (int i = 7; i >= 0; i--) {
		char c = "0123456789abcdef"[(v >> (4*i)) & 15];
		if (c == '0' && i >= digits) continue;
		putchar(c);
		digits = i;
	}
}

void memtest(uint32_t addr_ptr, uint32_t mem_total)
{
   int cyc_count = 5;
   int stride = 256;
   uint32_t state;

   volatile uint32_t *base_word = (uint32_t *) addr_ptr;
   volatile uint8_t *base_byte = (uint8_t *) addr_ptr;

   print("testing ");
   print_hex(mem_total, 8);
	print(" memory @ ");
   print_hex(addr_ptr, 8);
   print(" ");

   // Walk in stride increments, word access
   for (int i = 1; i <= cyc_count; i++) {
      state = i;

      for (int word = 0; word < mem_total / sizeof(int); word += stride) {
         *(base_word + word) = xorshift32(&state);
      }

      state = i;

      for (int word = 0; word < mem_total / sizeof(int); word += stride) {
         if (*(base_word + word) != xorshift32(&state)) {
            print(" ***FAILED WORD*** at ");
            print_hex(4 * word, 8);
				print(" CYC=");
            print_hex(i, 2);
            print("\n");
            return;
         }
      }

      print(".");
   }

   // Byte access
   for (int byte = 0; byte < 128; byte++) {
      *(base_byte + byte) = (uint8_t) byte;
   }

   for (int byte = 0; byte < 128; byte++) {
      if (*(base_byte + byte) != (uint8_t) byte) {
         print(" ***FAILED BYTE*** at ");
         print_hex(byte, 8);
         print("\n");
         return;
      }
   }

   print(" passed\n");
}

uint32_t xorshift32(uint32_t *state)
{
   /* Algorithm "xor" from p. 4 of Marsaglia, "Xorshift RNGs" */
   uint32_t x = *state;
   x ^= x << 13;
   x ^= x >> 17;
   x ^= x << 5;
   *state = x;

   return x;
}
