#include <stdint.h>

// Register offsets - using 32-bit access
#define gpu_x0 (*(volatile uint32_t*)0xa0000000)
#define gpu_y0 (*(volatile uint32_t*)0xa0000004)
#define gpu_x1 (*(volatile uint32_t*)0xa0000008)
#define gpu_y1 (*(volatile uint32_t*)0xa000000c)
#define gpu_color (*(volatile uint32_t*)0xa0000010)
#define gpu_start (*(volatile uint32_t*)0xa0000014)
#define gpu_busy (*(volatile uint32_t*)0xa0000018)
#define gpu_pixel_count (*(volatile uint32_t*)0xa000001c)
#define gpu_debug_cur_x (*(volatile uint32_t*)0xa0000020)
#define gpu_debug_cur_y (*(volatile uint32_t*)0xa0000024)  
#define gpu_debug_state_history (*(volatile uint32_t*)0xa0000028)

// UART output
#define uart_tx (*(volatile uint8_t*)0xf0000000)

void delay(void) {
    for (volatile int i = 0; i < 1000; i++);
}

void uart_putc(char c) {
    uart_tx = c;
    delay();
}

void uart_puts(const char *s) {
    while (*s) {
        uart_putc(*s++);
    }
}

void uart_putnum(uint32_t n) {
    char buf[12];
    int i = 0;
    if (n == 0) {
        uart_putc('0');
        return;
    }
    while (n > 0 && i < 11) {
        buf[i++] = '0' + (n % 10);
        n /= 10;
    }
    while (i--) uart_putc(buf[i]);
}

// Draw line with timeout
void draw_line(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint8_t color) {
    uart_puts("Drawing line (");
    uart_putnum(x0);
    uart_puts(",");
    uart_putnum(y0);
    uart_puts(") to (");
    uart_putnum(x1);
    uart_puts(",");
    uart_putnum(y1);
    uart_puts(") color=");
    uart_putnum(color);
    uart_puts("... ");

    gpu_x0 = x0;
    gpu_y0 = y0;
    gpu_x1 = x1;
    gpu_y1 = y1;
    gpu_color = color & 1;
    gpu_start = 1;

    // Wait until GPU is not busy with timeout
    int timeout = 100000;
    while ((gpu_busy & 1) && timeout > 0) {
        timeout--;
        delay();
    }
    
    if (timeout == 0) {
        uart_puts("TIMEOUT!\r\n");
    } else {
        uart_puts("DONE (");
        uart_putnum(gpu_pixel_count);
        uart_puts(" pixels) final_pos=(");
        uart_putnum(gpu_debug_cur_x);
        uart_puts(",");
        uart_putnum(gpu_debug_cur_y);
        uart_puts(") states=0x");
        
        // Print state history as hex
        uint32_t states = gpu_debug_state_history;
        for (int i = 4; i >= 0; i -= 4) {
            uint8_t digit = (states >> i) & 0xF;
            if (digit < 10)
                uart_putc('0' + digit);
            else
                uart_putc('A' + digit - 10);
        }
        uart_puts("\r\n");
    }
}

// Draw a box (rectangle) given top-left and bottom-right corners
void draw_box(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint8_t color) {
    uart_puts("=== Drawing Box ===\r\n");
    
    // Top edge
    draw_line(x0, y0, x1, y0, color);
    
    // Right edge  
    draw_line(x1, y0, x1, y1, color);
    
    // Bottom edge
    draw_line(x1, y1, x0, y1, color);
    
    // Left edge
    draw_line(x0, y1, x0, y0, color);
}

int main(void) {
    uart_puts("=== GPU Line Drawing Box Test ===\r\n");

    // Draw a 100x80 box starting at (50,50)
    draw_box(50, 50, 150, 130, 1);
    
    uart_puts("=== Drawing Diagonal Inside Box ===\r\n");
    
    // Draw diagonal from top-left to bottom-right
    draw_line(50, 50, 150, 130, 1);
    
    uart_puts("=== Drawing Second Diagonal ===\r\n");
    
    // Draw diagonal from top-right to bottom-left
    draw_line(150, 50, 50, 130, 1);

    uart_puts("=== Box Test Complete ===\r\n");
    
    uart_puts("=== Additional Line Tests ===\r\n");
    
    // Test some additional lines for verification
    // Horizontal line
    draw_line(200, 100, 300, 100, 1);
    
    // Vertical line
    draw_line(250, 50, 250, 150, 1);
    
    // Long diagonal
    draw_line(200, 200, 400, 300, 1);

    uart_puts("=== All Tests Complete ===\r\n");

    while (1);  // Idle
}
