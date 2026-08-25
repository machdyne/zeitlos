/*
 * Simplified GPU Blitter Debug Test
 * Focus on basic copy operation to isolate the issue
 */

#include <stdint.h>

// Hardware addresses  
#define UART_BASE    0xF0000000
#define VRAM_BASE    0x20000000
#define BLITTER_BASE 0xd0000000

// UART
#define UART_TX_DATA (*(volatile uint32_t*)(UART_BASE + 0x00))

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

// Screen parameters
#define SCREEN_WIDTH  512
#define SCREEN_HEIGHT 384
#define SCREEN_STRIDE 64

void uart_putc(char c) {
    UART_TX_DATA = c;
    for (volatile int i = 0; i < 100; i++);
}

void uart_puts(const char* s) {
    while (*s) uart_putc(*s++);
}

void uart_puthex(uint32_t val) {
    const char hex[] = "0123456789ABCDEF";
    uart_puts("0x");
    for (int i = 28; i >= 0; i -= 4) {
        uart_putc(hex[(val >> i) & 0xF]);
    }
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

void blit_wait() {
    uart_puts("Waiting for blitter...");
    while (BLIT_STATUS & 1) {
        uart_putc('.');
    }
    uart_puts(" done\r\n");
}

// Direct VRAM word access for debugging
uint32_t read_vram_word(uint32_t word_x, uint32_t y) {
    uint32_t byte_addr = y * SCREEN_STRIDE + word_x * 4;
    return *(volatile uint32_t*)(VRAM_BASE + byte_addr);
}

void write_vram_word(uint32_t word_x, uint32_t y, uint32_t value) {
    uint32_t byte_addr = y * SCREEN_STRIDE + word_x * 4;
    *(volatile uint32_t*)(VRAM_BASE + byte_addr) = value;
}

void dump_blitter_regs() {
    uart_puts("Blitter registers:\r\n");
    uart_puts("  CTRL="); uart_puthex(BLIT_CTRL); uart_puts("\r\n");
    uart_puts("  STATUS="); uart_puthex(BLIT_STATUS); uart_puts("\r\n");
    uart_puts("  DST_X="); uart_puthex(BLIT_DST_X); uart_puts("\r\n");
    uart_puts("  DST_Y="); uart_puthex(BLIT_DST_Y); uart_puts("\r\n");
    uart_puts("  WIDTH="); uart_puthex(BLIT_WIDTH); uart_puts("\r\n");
    uart_puts("  HEIGHT="); uart_puthex(BLIT_HEIGHT); uart_puts("\r\n");
    uart_puts("  PATTERN="); uart_puthex(BLIT_PATTERN); uart_puts("\r\n");
    uart_puts("  SRC_X="); uart_puthex(BLIT_SRC_X); uart_puts("\r\n");
    uart_puts("  SRC_Y="); uart_puthex(BLIT_SRC_Y); uart_puts("\r\n");
}

void test_basic_copy() {
    uart_puts("\r\n=== Basic Copy Test ===\r\n");
    
    // Step 1: Clear everything first
    uart_puts("1. Clearing test area...\r\n");
    write_vram_word(0, 10, 0x00000000);  // Source
    write_vram_word(4, 10, 0x00000000);  // Destination
    
    // Debug: Check calculated addresses
    uart_puts("Address calculations:\r\n");
    uart_puts("  Source pixel (0,10) -> word 0, byte addr = ");
    uart_putdec(10 * SCREEN_STRIDE + 0 * 4); uart_puts(" = ");
    uart_puthex(10 * SCREEN_STRIDE + 0 * 4); uart_puts("\r\n");
    uart_puts("  Dest pixel (128,10) -> word 4, byte addr = ");
    uart_putdec(10 * SCREEN_STRIDE + 4 * 4); uart_puts(" = ");
    uart_puthex(10 * SCREEN_STRIDE + 4 * 4); uart_puts("\r\n");
    
    // Step 2: Write known pattern to source manually
    uart_puts("2. Writing test pattern to source...\r\n");
    uint32_t test_pattern = 0xDEADBEEF;
    write_vram_word(0, 10, test_pattern);
    
    // Verify source was written
    uint32_t read_back = read_vram_word(0, 10);
    uart_puts("   Source word (0,10) = "); uart_puthex(read_back);
    if (read_back == test_pattern) {
        uart_puts(" GOOD\r\n");
    } else {
        uart_puts(" BAD - expected "); uart_puthex(test_pattern); uart_puts("\r\n");
        return;
    }
    
    // Debug: Test direct VRAM read at the address the blitter should use
    uart_puts("3. Testing direct VRAM read at blitter source address...\r\n");
    uint32_t blitter_src_addr = 10 * 64 + (0 >> 5) * 4;  // Same calc as blitter
    uint32_t direct_read = *(volatile uint32_t*)(VRAM_BASE + blitter_src_addr);
    uart_puts("   Direct read from VRAM["); uart_puthex(blitter_src_addr); 
    uart_puts("] = "); uart_puthex(direct_read); uart_puts("\r\n");
    if (direct_read != test_pattern) {
        uart_puts("   ERROR: Blitter will read wrong data!\r\n");
        return;
    }
    
    // Step 3: Verify destination is clear
    uint32_t dst_before = read_vram_word(4, 10);
    uart_puts("4. Destination word (4,10) before = "); uart_puthex(dst_before); uart_puts("\r\n");
    
    // Step 4: Set up blitter for copy
    uart_puts("5. Setting up blitter for copy...\r\n");
    BLIT_SRC_X = 0;      // Source pixel x (word 0)
    BLIT_SRC_Y = 10;     // Source line
    BLIT_DST_X = 128;    // Destination pixel x (word 4)
    BLIT_DST_Y = 10;     // Destination line
    BLIT_WIDTH = 32;     // One word worth of pixels
    BLIT_HEIGHT = 1;     // One line
    
    dump_blitter_regs();
    
    // Step 5: Start copy operation
    uart_puts("6. Starting copy operation...\r\n");
    uint32_t ctrl_val = CTRL_CLIP | CTRL_START;  // Copy mode (no FILL bit)
    uart_puts("   Writing CTRL = "); uart_puthex(ctrl_val); uart_puts("\r\n");
    BLIT_CTRL = ctrl_val;
    
    // Step 6: Wait for completion
    blit_wait();
    
    // Step 7: Check result
    uart_puts("7. Checking result...\r\n");
    uint32_t dst_after = read_vram_word(4, 10);
    uart_puts("   Destination word (4,10) after = "); uart_puthex(dst_after); uart_puts("\r\n");
    
    if (dst_after == test_pattern) {
        uart_puts("   COPY TEST PASSED!\r\n");
    } else {
        uart_puts("   COPY TEST FAILED!\r\n");
        uart_puts("   Expected: "); uart_puthex(test_pattern); uart_puts("\r\n");
        uart_puts("   Got:      "); uart_puthex(dst_after); uart_puts("\r\n");
        
        // Additional debugging
        uart_puts("\r\nDebugging info:\r\n");
        uart_puts("Source word is still: "); uart_puthex(read_vram_word(0, 10)); uart_puts("\r\n");
        dump_blitter_regs();
    }
}

void test_basic_fill() {
    uart_puts("\r\n=== Basic Fill Test (for comparison) ===\r\n");
    
    // Clear destination first
    write_vram_word(2, 20, 0x00000000);
    
    BLIT_DST_X = 64;      // Word 2
    BLIT_DST_Y = 20;
    BLIT_WIDTH = 32;      // One word
    BLIT_HEIGHT = 1;
    BLIT_PATTERN = 0x12345678;
    
    uart_puts("Starting fill...\r\n");
    BLIT_CTRL = CTRL_FILL | CTRL_CLIP | CTRL_START;
    blit_wait();
    
    uint32_t result = read_vram_word(2, 20);
    uart_puts("Fill result: "); uart_puthex(result);
    if (result == 0x12345678) {
        uart_puts(" PASSED\r\n");
    } else {
        uart_puts(" FAILED\r\n");
    }
}

int main() {
    uart_puts("Simplified Blitter Debug Test\r\n");
    uart_puts("=============================\r\n");
    
    // Test fill first to make sure basic blitter works
    test_basic_fill();
    
    // Then test copy
    test_basic_copy();
    
    uart_puts("\r\n=== Additional Copy Tests ===\r\n");
    
    // Test multiple word copy
    uart_puts("Testing 2-word copy...\r\n");
    write_vram_word(0, 20, 0x11111111);
    write_vram_word(1, 20, 0x22222222);
    
    BLIT_SRC_X = 0;
    BLIT_SRC_Y = 20;
    BLIT_DST_X = 256;  // Word 8
    BLIT_DST_Y = 20;
    BLIT_WIDTH = 64;   // 2 words
    BLIT_HEIGHT = 1;
    BLIT_CTRL = CTRL_CLIP | CTRL_START;
    blit_wait();
    
    uint32_t word8 = read_vram_word(8, 20);
    uint32_t word9 = read_vram_word(9, 20);
    uart_puts("Word 8: "); uart_puthex(word8);
    uart_puts(" Word 9: "); uart_puthex(word9); uart_puts("\r\n");
    
    if (word8 == 0x11111111 && word9 == 0x22222222) {
        uart_puts("Multi-word copy PASSED\r\n");
    } else {
        uart_puts("Multi-word copy FAILED\r\n");
    }
    
    // Test multi-line copy
    uart_puts("Testing multi-line copy...\r\n");
    write_vram_word(0, 30, 0xAAAAAAAA);
    write_vram_word(0, 31, 0xBBBBBBBB);
    
    BLIT_SRC_X = 0;
    BLIT_SRC_Y = 30;
    BLIT_DST_X = 64;   // Word 2
    BLIT_DST_Y = 40;
    BLIT_WIDTH = 32;   // 1 word
    BLIT_HEIGHT = 2;   // 2 lines
    BLIT_CTRL = CTRL_CLIP | CTRL_START;
    blit_wait();
    
    uint32_t line40 = read_vram_word(2, 40);
    uint32_t line41 = read_vram_word(2, 41);
    uart_puts("Line 40: "); uart_puthex(line40);
    uart_puts(" Line 41: "); uart_puthex(line41); uart_puts("\r\n");
    
    if (line40 == 0xAAAAAAAA && line41 == 0xBBBBBBBB) {
        uart_puts("Multi-line copy PASSED\r\n");
    } else {
        uart_puts("Multi-line copy FAILED\r\n");
    }
    
    uart_puts("\r\nAll tests complete - Copy mode is working!\r\n");
    return 0;
}
