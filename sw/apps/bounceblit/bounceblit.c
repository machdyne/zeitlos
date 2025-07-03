#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "../../common/zeitlos.h"

// Memory-mapped addresses
#define BLITTER_BASE    0xD0000000

// Clipping Blitter registers
#define BLIT_CTRL       (*(volatile uint32_t*)(BLITTER_BASE + 0x00))
#define BLIT_STATUS     (*(volatile uint32_t*)(BLITTER_BASE + 0x04))
#define BLIT_DST_X      (*(volatile uint32_t*)(BLITTER_BASE + 0x08))
#define BLIT_DST_Y      (*(volatile uint32_t*)(BLITTER_BASE + 0x0C))
#define BLIT_WIDTH      (*(volatile uint32_t*)(BLITTER_BASE + 0x10))
#define BLIT_HEIGHT     (*(volatile uint32_t*)(BLITTER_BASE + 0x14))
#define BLIT_PATTERN    (*(volatile uint32_t*)(BLITTER_BASE + 0x18))

// Control bits
#define CTRL_START      (1 << 0)
#define CTRL_FILL       (1 << 1)
#define CTRL_CLIP       (1 << 2)

// Screen parameters
#define WIDTH           512
#define HEIGHT          384

// Patterns
#define PATTERN_BLACK   0x00000000
#define PATTERN_WHITE   0xFFFFFFFF

void delay() {
    volatile static int x, y;
    for (int i = 0; i < 5000; i++) {
        x += y;
    }
}

// Wait for blitter completion
void wait_blitter_done(void) {
    while (BLIT_STATUS & 1) {
        // Wait for busy flag to clear
    }
}

// Fill rectangle with clipping
void fill_rect(int x, int y, int width, int height, uint32_t pattern) {
    BLIT_DST_X = x;
    BLIT_DST_Y = y;
    BLIT_WIDTH = width;
    BLIT_HEIGHT = height;
    BLIT_PATTERN = pattern;
    BLIT_CTRL = CTRL_START | CTRL_FILL | CTRL_CLIP;
    
    wait_blitter_done();
}

// Clear screen
void clear_screen(void) {
    BLIT_DST_X = 0;
    BLIT_DST_Y = 0;
    BLIT_WIDTH = WIDTH;
    BLIT_HEIGHT = HEIGHT;
    BLIT_PATTERN = PATTERN_BLACK;
    BLIT_CTRL = CTRL_START | CTRL_FILL;  // No clipping needed for full screen
    
    wait_blitter_done();
}

int main() {
    // Clear screen
    clear_screen();
    
    // Bouncing box state
    int x = 0, y = 0;
    int dx = 1, dy = 1;
    const int box_size = 10;
    
    while (1) {
        // Erase previous box
        fill_rect(x, y, box_size, box_size, PATTERN_BLACK);
        
        // Update position
        x += dx;
        y += dy;
        
        // Bounce off walls
        if (x <= 0 || x + box_size >= WIDTH) dx = -dx;
        if (y <= 0 || y + box_size >= HEIGHT) dy = -dy;
        
        // Draw new box
        fill_rect(x, y, box_size, box_size, PATTERN_WHITE);
        
        // Control speed
        delay();
    }
    
    return 0;
}
