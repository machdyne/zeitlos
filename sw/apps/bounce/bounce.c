#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "../../common/zeitlos.h"

#define MEM_VRAM ((volatile uint32_t *)0x20000000)
#define WIDTH 512
#define HEIGHT 384

void set_pixel(int x, int y, int c);
void draw_box(int x0, int y0, int width, int height, int c);
void draw_line(int x0, int y0, int x1, int y1);

void delay() {
   volatile static int x, y;
   for (int i = 0; i < 5000; i++) {
      x += y;
   }
}

int main() {

	int x = 0, y = 0;
	int dx = 1, dy = 1;
	const int box_size = 10;

	while (1) {

		// erase previous box
		draw_box(x, y, box_size, box_size, 0);

		// update position
		x += dx;
		y += dy;

		// bounce off walls
		if (x <= 0 || x + box_size >= WIDTH) dx = -dx;
		if (y <= 0 || y + box_size >= HEIGHT) dy = -dy;

		// draw new box
		draw_box(x, y, box_size, box_size, 1);

		// delay to control speed
		delay();
	}

	return 0;

}

void set_pixel(int x, int y, int c) {

    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) {
        return; // out of bounds
    }

    uint32_t bit_index = y * WIDTH + x;
    uint32_t word_index = bit_index / 32;
    uint32_t bit_position = bit_index % 32;

   // uint32_t mask = 1U << (31 - bit_position);
    uint32_t mask = 1U << bit_position;

    if (c) {
        MEM_VRAM[word_index] |= mask;  // set
    } else {
        MEM_VRAM[word_index] &= ~mask; // clear
    }

}

void draw_box(int x0, int y0, int width, int height, int c) {
	for (int y = y0; y < y0 + height; y++) {
		for (int x = x0; x < x0 + width; x++) {
			set_pixel(x, y, c);
		}
	}
}
void draw_line(int x0, int y0, int x1, int y1) {

	for (float t = 0.0; t < 1.0; t+= 0.01) {
		int x = x0 + (x1 - x0) * t;
		int y = y0 + (y1 - y0) * t;
		set_pixel(x, y, 1);
	}

}
