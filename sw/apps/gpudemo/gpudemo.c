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
#define gpu_debug_fifo_count (*(volatile uint32_t*)0xa0000028)

#define gpu_clip_x0     (*(volatile uint32_t*)0xa000002c)  // Left bound
#define gpu_clip_y0     (*(volatile uint32_t*)0xa0000030)  // Top bound  
#define gpu_clip_x1     (*(volatile uint32_t*)0xa0000034)  // Right bound
#define gpu_clip_y1     (*(volatile uint32_t*)0xa0000038)  // Bottom bound
#define gpu_clip_enable (*(volatile uint32_t*)0xa000003c)  // Enable clipping

#define reg_usb_cursor (*(volatile uint32_t*)0xc000000c)

// UART output
#define uart_tx (*(volatile uint8_t*)0xf0000000)


uint16_t get_cursor_x() {
    return reg_usb_cursor & 0x3FF; // bits 9:0
}

uint16_t get_cursor_y() {
    return (reg_usb_cursor >> 10) & 0x3FF; // bits 19:10
}

uint8_t get_mouse_btn() {
    return (reg_usb_cursor >> 20) & 0x0F; // bits 23:20
}

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
/*
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
*/

   // while ((gpu_busy & 1)) /* wait */;
    while (gpu_debug_fifo_count > 15) /* wait */;

    gpu_x0 = x0;
    gpu_y0 = y0;
    gpu_x1 = x1;
    gpu_y1 = y1;
    gpu_color = color & 1;
    gpu_start = 1;

/*
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
*/

}

// Draw a box (rectangle) given top-left and bottom-right corners
void draw_box(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint8_t color) {
    
    // Top edge
    draw_line(x0, y0, x1, y0, color);
    
    // Right edge  
    draw_line(x1, y0, x1, y1, color);
    
    // Bottom edge
    draw_line(x1, y1, x0, y1, color);
    
    // Left edge
    draw_line(x0, y1, x0, y0, color);

}

void test_fifo_burst() {
    uart_puts("=== FIFO Burst Test ===\r\n");
    
    uint32_t fifo_counts[10];
    
    // Issue commands and capture FIFO counts immediately
    for (int i = 0; i < 10; i++) {
        gpu_x0 = i * 10;
        gpu_y0 = i * 10; 
        gpu_x1 = i * 10 + 50;
        gpu_y1 = i * 10 + 50;
        gpu_color = 1;
        gpu_start = 1;
        
        // Read FIFO count IMMEDIATELY after command
        fifo_counts[i] = gpu_debug_fifo_count;
    }
    
    // Now print the captured values
    for (int i = 0; i < 10; i++) {
        uart_puts("Cmd ");
        uart_putnum(i);
        uart_puts(" FIFO was ");
        uart_putnum(fifo_counts[i]);
        uart_puts("\r\n");
    }
}

int main(void) {
    uart_puts("=== GPU Line Drawing Box Test ===\r\n");

    uart_puts("=== Drawing Box ===\r\n");

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

	test_fifo_burst();

    uart_puts("=== All Tests Complete ===\r\n");


    uart_puts("=== Enabling clipping ===\r\n");

gpu_clip_x0 = 100;
gpu_clip_y0 = 50; 
gpu_clip_x1 = 299;  // inclusive bounds
gpu_clip_y1 = 199;
gpu_clip_enable = 1;

	uint16_t x, y;
	uint16_t lx, ly;

	uint32_t c = 0;

	while (1) {

		lx = x;
		ly = y;

		x = get_cursor_x() / 2;
		y = get_cursor_y() / 2;

		if (x != lx || y != ly) {
			draw_box(lx, ly, lx + 200, ly + 100, 0);
			draw_box(x, y, x + 200, y + 100, 1);

		}
/*
		if ((c++ % 10000) == 0) {
        uart_puts("FIFO = ");
        uart_putnum(gpu_debug_fifo_count);
        uart_puts("\r\n");
		}
*/
		c++;

	}

}
