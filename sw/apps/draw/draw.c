/*
 * Clean Blitter Paint Program
 * Updated for standard monochrome framebuffer layout
 */

#include <stdint.h>

// Hardware addresses
#define UART_BASE    0xF0000000
#define VRAM_BASE    0x20000000
#define BLITTER_BASE 0xd0000000

// UART
#define UART_TX_DATA (*(volatile uint32_t*)(UART_BASE + 0x00))

// Mouse interface
#define reg_usb_cursor (*(volatile uint32_t*)0xc000000c)
uint16_t get_cursor_x() {
    return reg_usb_cursor & 0x3FF; // bits 9:0
}
uint16_t get_cursor_y() {
    return (reg_usb_cursor >> 10) & 0x3FF; // bits 19:10
}
uint8_t get_mouse_btn() {
    return (reg_usb_cursor >> 20) & 0x0F; // bits 23:20
}
#define MOUSE_BUTTON_LEFT  (1 << 0)

// Blitter registers
#define BLIT_CTRL    (*(volatile uint32_t*)(BLITTER_BASE + 0x00))
#define BLIT_STATUS  (*(volatile uint32_t*)(BLITTER_BASE + 0x04))
#define BLIT_DST_X   (*(volatile uint32_t*)(BLITTER_BASE + 0x08))
#define BLIT_DST_Y   (*(volatile uint32_t*)(BLITTER_BASE + 0x0C))
#define BLIT_WIDTH   (*(volatile uint32_t*)(BLITTER_BASE + 0x10))
#define BLIT_HEIGHT  (*(volatile uint32_t*)(BLITTER_BASE + 0x14))
#define BLIT_PATTERN (*(volatile uint32_t*)(BLITTER_BASE + 0x18))
#define BLIT_SRC_X   (*(volatile uint32_t*)(BLITTER_BASE + 0x1C))
#define BLIT_SRC_Y   (*(volatile uint32_t*)(BLITTER_BASE + 0x20))

// Control bits
#define CTRL_START (1 << 0)
#define CTRL_FILL  (1 << 1)
#define CTRL_CLIP  (1 << 2)

// Screen parameters - Standard framebuffer with GPU_PIXEL_DOUBLE
#define SCREEN_WIDTH  512
#define SCREEN_HEIGHT 384
#define SCREEN_STRIDE 64  // 512 pixels / 8 = 64 bytes per line

// Layout
#define CANVAS_HEIGHT 300      // Drawing area
#define PALETTE_HEIGHT 84      // Brush area  
#define PALETTE_Y CANVAS_HEIGHT

// Brush settings
#define BRUSH_COUNT 4          // Clean set of 4 brushes
#define BRUSH_SIZE 32          // 32x32 pixels each
#define BRUSH_SPACING 64       // 64 pixels apart (word-aligned)

// Global state
int selected_brush = 0;
uint16_t last_mouse_x = 0, last_mouse_y = 0;
uint8_t last_buttons = 0;

// Simple UART functions
void uart_putc(char c) {
    UART_TX_DATA = c;
    for (volatile int i = 0; i < 100; i++);
}

void uart_puts(const char* s) {
    while (*s) uart_putc(*s++);
}

void uart_putdec(uint32_t val) {
    char buf[16];
    int i = 0;
    if (val == 0) { uart_putc('0'); return; }
    while (val > 0) {
        buf[i++] = '0' + (val % 10);
        val /= 10;
    }
    while (i > 0) uart_putc(buf[--i]);
}

void uart_puthex(uint32_t val) {
    const char hex[] = "0123456789ABCDEF";
    uart_puts("0x");
    for (int i = 28; i >= 0; i -= 4) {
        uart_putc(hex[(val >> i) & 0xF]);
    }
}

// Blitter helpers
void blit_wait() {
    while (BLIT_STATUS & 1);
}

void blit_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t pattern) {
    BLIT_DST_X = x;
    BLIT_DST_Y = y;
    BLIT_WIDTH = w;
    BLIT_HEIGHT = h;
    BLIT_PATTERN = pattern;
    BLIT_CTRL = CTRL_FILL | CTRL_CLIP | CTRL_START;
    blit_wait();
}

void blit_copy(uint32_t src_x, uint32_t src_y, uint32_t dst_x, uint32_t dst_y, uint32_t w, uint32_t h) {
    BLIT_SRC_X = src_x;
    BLIT_SRC_Y = src_y;
    BLIT_DST_X = dst_x;
    BLIT_DST_Y = dst_y;
    BLIT_WIDTH = w;
    BLIT_HEIGHT = h;
    BLIT_CTRL = CTRL_CLIP | CTRL_START;  // Copy mode
    blit_wait();
}

// Direct VRAM access - now uses standard framebuffer layout
void write_vram_word(uint32_t word_x, uint32_t y, uint32_t value) {
    uint32_t byte_addr = y * SCREEN_STRIDE + word_x * 4;
    *(volatile uint32_t*)(VRAM_BASE + byte_addr) = value;
}

uint32_t read_vram_word(uint32_t word_x, uint32_t y) {
    uint32_t byte_addr = y * SCREEN_STRIDE + word_x * 4;
    return *(volatile uint32_t*)(VRAM_BASE + byte_addr);
}

// Create 4 distinct brush patterns using standard framebuffer layout
void create_brushes() {
    uart_puts("Creating brushes for standard framebuffer...\r\n");
    
    for (int brush = 0; brush < BRUSH_COUNT; brush++) {
        uint32_t brush_x = brush * BRUSH_SPACING;  // Positions: 0, 64, 128, 192
        uint32_t brush_y = PALETTE_Y + 32;
        
        uart_puts("Brush "); uart_putdec(brush); uart_puts(" at (");
        uart_putdec(brush_x); uart_puts(","); uart_putdec(brush_y); uart_puts(")\r\n");
        
        // Clear area first
        blit_fill(brush_x, brush_y, BRUSH_SIZE, BRUSH_SIZE, 0x00000000);
        
        // Create patterns - now in logical order since framebuffer is fixed
        switch (brush) {
            case 0: // Leftmost - Solid white
                for (int y = 0; y < BRUSH_SIZE; y++) {
                    write_vram_word(brush_x / 32, brush_y + y, 0xFFFFFFFF);
                }
                break;
                
            case 1: // Second - Vertical stripes
                for (int y = 0; y < BRUSH_SIZE; y++) {
                    write_vram_word(brush_x / 32, brush_y + y, 0xAAAAAAAA);
                }
                break;
                
            case 2: // Third - Horizontal stripes
                for (int y = 0; y < BRUSH_SIZE; y++) {
                    write_vram_word(brush_x / 32, brush_y + y, (y & 1) ? 0xFFFFFFFF : 0x00000000);
                }
                break;
                
            case 3: // Rightmost - Checkerboard
                for (int y = 0; y < BRUSH_SIZE; y++) {
                    write_vram_word(brush_x / 32, brush_y + y, (y & 1) ? 0x55555555 : 0xAAAAAAAA);
                }
                break;
        }
        
        // Verify pattern was created correctly
        uint32_t test = read_vram_word(brush_x / 32, brush_y);
        uart_puts("  Pattern: "); uart_puthex(test); uart_puts("\r\n");
    }
    
    uart_puts("Brushes created in standard layout.\r\n");
}

// Draw selection indicator below the selected brush
void update_selection() {
    // Clear all selection indicators
    for (int i = 0; i < BRUSH_COUNT; i++) {
        uint32_t x = i * BRUSH_SPACING;
        uint32_t y = PALETTE_Y + 32 + BRUSH_SIZE + 2;  // Below brush
        blit_fill(x, y, BRUSH_SIZE, 6, 0x00000000);
    }
    
    // Draw indicator below selected brush
    uint32_t sel_x = selected_brush * BRUSH_SPACING;
    uint32_t sel_y = PALETTE_Y + 32 + BRUSH_SIZE + 2;
    blit_fill(sel_x, sel_y, BRUSH_SIZE, 6, 0xFFFFFFFF);
    
    uart_puts("Selected brush "); uart_putdec(selected_brush); uart_puts("\r\n");
}

// Setup the interface
void setup() {
    uart_puts("Setting up standard framebuffer paint program...\r\n");
    
    // Clear screen
    blit_fill(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, 0x00000000);
    
    // Draw separator line
    blit_fill(0, CANVAS_HEIGHT, SCREEN_WIDTH, 2, 0xFFFFFFFF);
    
    // Create brush patterns
    create_brushes();
    
    // Show initial selection
    update_selection();
    
    uart_puts("Setup complete! Brushes should appear left to right: White, Vertical, Horizontal, Checker\r\n");
}

// Handle brush selection
void select_brush(uint16_t mouse_x, uint16_t mouse_y) {
    uart_puts("Brush area click at ("); uart_putdec(mouse_x); uart_puts(","); uart_putdec(mouse_y); uart_puts(")\r\n");
    
    // Check which brush was clicked
    for (int i = 0; i < BRUSH_COUNT; i++) {
        uint32_t brush_x = i * BRUSH_SPACING;
        if (mouse_x >= brush_x && mouse_x < brush_x + BRUSH_SIZE) {
            selected_brush = i;
            update_selection();
            return;
        }
    }
}

// Draw with current brush
void draw_brush(uint16_t mouse_x, uint16_t mouse_y) {
    // Calculate source position (where the brush preview is)
    uint32_t src_x = selected_brush * BRUSH_SPACING;
    uint32_t src_y = PALETTE_Y + 32;
    
    // Calculate destination (where to draw)
    int32_t dst_x = (int32_t)mouse_x - BRUSH_SIZE/2;
    int32_t dst_y = (int32_t)mouse_y - BRUSH_SIZE/2;
    
    // Copy brush pattern to canvas using blitter
    blit_copy(src_x, src_y, dst_x, dst_y, BRUSH_SIZE, BRUSH_SIZE);
}

// Handle mouse input
void handle_mouse() {
    uint16_t raw_x = get_cursor_x();
    uint16_t raw_y = get_cursor_y();
    uint8_t buttons = get_mouse_btn();
    
    // Scale coordinates (cursor reports 1024x768, display is 512x384)
    uint16_t mouse_x = raw_x / 2;
    uint16_t mouse_y = raw_y / 2;
    
    // Clamp to screen bounds
    if (mouse_x >= SCREEN_WIDTH) mouse_x = SCREEN_WIDTH - 1;
    if (mouse_y >= SCREEN_HEIGHT) mouse_y = SCREEN_HEIGHT - 1;
    
    // Detect button press
    uint8_t pressed = buttons & ~last_buttons;
    
    if (pressed & MOUSE_BUTTON_LEFT) {
        if (mouse_y >= PALETTE_Y) {
            // Click in brush area - select brush
            select_brush(mouse_x, mouse_y);
        } else {
            // Click in canvas area - draw
            draw_brush(mouse_x, mouse_y);
        }
    }
    
    // Handle dragging in canvas
    if ((buttons & MOUSE_BUTTON_LEFT) && (mouse_y < CANVAS_HEIGHT)) {
        int32_t dx = (int32_t)mouse_x - (int32_t)last_mouse_x;
        int32_t dy = (int32_t)mouse_y - (int32_t)last_mouse_y;
        if (dx*dx + dy*dy > 25) {  // Moved enough to draw
            draw_brush(mouse_x, mouse_y);
        }
    }
    
    last_mouse_x = mouse_x;
    last_mouse_y = mouse_y;
    last_buttons = buttons;
}

int main() {
    uart_puts("Standard Framebuffer Paint Program\r\n");
    uart_puts("==================================\r\n");
    uart_puts("Now using corrected GPU video with standard monochrome framebuffer layout.\r\n");
    uart_puts("4 brushes at bottom: White, Vertical, Horizontal, Checker (left to right)\r\n");
    uart_puts("Click to select brush, draw in top area.\r\n\r\n");
    
    setup();
    
    while (1) {
        handle_mouse();
        for (volatile int i = 0; i < 1000; i++);  // Small delay
    }
    
    return 0;
}
