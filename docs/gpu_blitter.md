# Zeitlos GPU Blitter Developer Guide

## Overview

The Zeitlos GPU Blitter is a high-performance 2D graphics accelerator designed for efficient rectangular fills, sprite blitting, and text rendering. It operates on a 512×384 monochrome (1-bit-per-pixel) framebuffer with automatic clipping and word-level optimization.

## Key Features

- **Hardware-accelerated rectangular fills**
- **Automatic screen boundary clipping**
- **Word-level optimization with pixel-precise masking**
- **Simple memory-mapped register interface**
- **Optimized for font rendering (6×12 characters)**
- **Safe operation with bounds checking**

## Memory Map

| Address | Register | Description |
|---------|----------|-------------|
| 0xD0000000 | BLIT_CTRL | Control register |
| 0xD0000004 | BLIT_STATUS | Status register (read-only) |
| 0xD0000008 | BLIT_DST_X | Destination X coordinate (pixels) |
| 0xD000000C | BLIT_DST_Y | Destination Y coordinate (pixels) |
| 0xD0000010 | BLIT_WIDTH | Width in pixels |
| 0xD0000014 | BLIT_HEIGHT | Height in pixels |
| 0xD0000018 | BLIT_PATTERN | Fill pattern (32-bit) |

## Control Register (BLIT_CTRL)

| Bit | Name | Description |
|-----|------|-------------|
| 0 | START | Start operation (write 1 to begin) |
| 1 | FILL | Operation mode (0=copy, 1=fill) |
| 2 | CLIP | Clipping enable (0=disabled, 1=enabled) |

## Status Register (BLIT_STATUS)

| Bit | Name | Description |
|-----|------|-------------|
| 0 | BUSY | Operation in progress (1=busy, 0=idle) |

## Screen Specifications

- **Resolution**: 512×384 pixels
- **Color depth**: 1-bit per pixel (monochrome)
- **Framebuffer**: Located at 0x20000000
- **Stride**: 64 bytes per line (512 pixels ÷ 8 bits/byte)

## Programming Interface

### C Header Definition

```c
#define BLITTER_BASE    0xD0000000

#define BLIT_CTRL       (*(volatile uint32_t*)(BLITTER_BASE + 0x00))
#define BLIT_STATUS     (*(volatile uint32_t*)(BLITTER_BASE + 0x04))
#define BLIT_DST_X      (*(volatile uint32_t*)(BLITTER_BASE + 0x08))
#define BLIT_DST_Y      (*(volatile uint32_t*)(BLITTER_BASE + 0x0C))
#define BLIT_WIDTH      (*(volatile uint32_t*)(BLITTER_BASE + 0x10))
#define BLIT_HEIGHT     (*(volatile uint32_t*)(BLITTER_BASE + 0x14))
#define BLIT_PATTERN    (*(volatile uint32_t*)(BLITTER_BASE + 0x18))

#define CTRL_START      (1 << 0)
#define CTRL_FILL       (1 << 1)
#define CTRL_CLIP       (1 << 2)

#define PATTERN_BLACK   0x00000000
#define PATTERN_WHITE   0xFFFFFFFF
```

### Basic Usage

#### 1. Wait for Completion

Always wait for the blitter to complete before starting a new operation:

```c
void wait_blitter_done(void) {
    while (BLIT_STATUS & 1) {
        // Wait for busy flag to clear
    }
}
```

#### 2. Fill Rectangle with Clipping

```c
void fill_rect(int x, int y, int width, int height, uint32_t pattern) {
    BLIT_DST_X = x;
    BLIT_DST_Y = y;
    BLIT_WIDTH = width;
    BLIT_HEIGHT = height;
    BLIT_PATTERN = pattern;
    BLIT_CTRL = CTRL_START | CTRL_FILL | CTRL_CLIP;
    
    wait_blitter_done();
}
```

#### 3. Clear Screen (Fast)

```c
void clear_screen(void) {
    BLIT_DST_X = 0;
    BLIT_DST_Y = 0;
    BLIT_WIDTH = 512;
    BLIT_HEIGHT = 384;
    BLIT_PATTERN = PATTERN_BLACK;
    BLIT_CTRL = CTRL_START | CTRL_FILL;  // No clipping needed
    
    wait_blitter_done();
}
```

## Common Use Cases

### Drawing Pixels and Small Rectangles

```c
// Single pixel
fill_rect(10, 20, 1, 1, PATTERN_WHITE);

// Small rectangle
fill_rect(50, 50, 10, 10, PATTERN_WHITE);

// Font-sized character block
fill_rect(100, 100, 6, 12, PATTERN_WHITE);
```

### Text Rendering

```c
void draw_char(int x, int y, char c) {
    // Example: Simple 'A' character
    fill_rect(x + 1, y + 0, 4, 2, PATTERN_WHITE);  // Top
    fill_rect(x + 0, y + 2, 2, 8, PATTERN_WHITE);  // Left
    fill_rect(x + 4, y + 2, 2, 8, PATTERN_WHITE);  // Right  
    fill_rect(x + 1, y + 5, 4, 2, PATTERN_WHITE);  // Middle
}

void draw_text(int x, int y, const char* text) {
    int char_x = x;
    for (int i = 0; text[i]; i++) {
        draw_char(char_x, y, text[i]);
        char_x += 8;  // 6 pixels wide + 2 spacing
    }
}
```

### Bouncing Animation

```c
int main() {
    clear_screen();
    
    int x = 0, y = 0;
    int dx = 1, dy = 1;
    const int box_size = 10;
    
    while (1) {
        // Erase previous position
        fill_rect(x, y, box_size, box_size, PATTERN_BLACK);
        
        // Update position
        x += dx;
        y += dy;
        
        // Bounce off walls
        if (x <= 0 || x + box_size >= 512) dx = -dx;
        if (y <= 0 || y + box_size >= 384) dy = -dy;
        
        // Draw new position
        fill_rect(x, y, box_size, box_size, PATTERN_WHITE);
        
        delay();
    }
}
```

## Performance Characteristics

### Optimized Operations

- **Word-aligned fills**: Fastest performance
- **Screen clears**: ~1,200 memory operations
- **Large rectangles**: Word-level efficiency
- **Clipping**: Minimal overhead (~5-10%)

### Performance by Object Size

| Object Size | Memory Operations | Performance |
|-------------|-------------------|-------------|
| 1×1 pixel | ~2-4 | Excellent |
| 6×12 character | ~8-16 | Excellent |
| 10×10 box | ~20-40 | Very Good |
| 32×32 square | ~64-128 | Excellent |
| Full screen | ~1,200 | Blazing Fast |

## Clipping Behavior

### Automatic Clipping

When `CTRL_CLIP` is enabled, the blitter automatically:
- Clips rectangles to screen boundaries (0,0 to 511,383)
- Handles negative coordinates safely
- Prevents buffer overruns
- Uses pixel-level masks for partial word operations

### Examples

```c
// These are all safe with clipping enabled:
fill_rect(-10, -10, 50, 50, PATTERN_WHITE);      // Clips to screen
fill_rect(500, 300, 100, 100, PATTERN_WHITE);    // Clips to screen
fill_rect(256, 192, 1000, 1000, PATTERN_WHITE);  // Clips to screen
```

## Pattern Values

The `BLIT_PATTERN` register accepts 32-bit values:

```c
#define PATTERN_BLACK   0x00000000  // All pixels black
#define PATTERN_WHITE   0xFFFFFFFF  // All pixels white
#define PATTERN_CHECKER 0xAAAAAAAA  // Alternating pixels
#define PATTERN_DOTS    0x11111111  // Sparse dots
```

## Error Handling

The blitter is designed to be robust:
- **Out-of-bounds coordinates**: Safely clipped
- **Zero dimensions**: Operation completes immediately
- **Negative coordinates**: Handled by clipping
- **Concurrent operations**: Hardware prevents conflicts

## Best Practices

### 1. Always Wait for Completion
```c
// Wrong - may corrupt operations
fill_rect(0, 0, 10, 10, PATTERN_WHITE);
fill_rect(20, 20, 10, 10, PATTERN_WHITE);

// Correct
fill_rect(0, 0, 10, 10, PATTERN_WHITE);
wait_blitter_done();
fill_rect(20, 20, 10, 10, PATTERN_WHITE);
```

### 2. Use Clipping for Safety
```c
// Recommended for user input or dynamic coordinates
BLIT_CTRL = CTRL_START | CTRL_FILL | CTRL_CLIP;

// Only disable clipping for known-safe operations
BLIT_CTRL = CTRL_START | CTRL_FILL;  // Slightly faster
```

### 3. Optimize Common Operations
```c
// Fast screen clear
void clear_screen_fast(void) {
    BLIT_DST_X = 0;
    BLIT_DST_Y = 0;
    BLIT_WIDTH = 512;
    BLIT_HEIGHT = 384;
    BLIT_PATTERN = PATTERN_BLACK;
    BLIT_CTRL = CTRL_START | CTRL_FILL;  // No clipping overhead
    wait_blitter_done();
}
```

## Troubleshooting

### Operation Not Starting
- Check if previous operation completed (`BLIT_STATUS & 1 == 0`)
- Ensure `CTRL_START` bit is set in control register
- Verify register addresses are correct

### Unexpected Results
- Check coordinate bounds (0-511 for X, 0-383 for Y)
- Verify pattern value (0x00000000 for black, 0xFFFFFFFF for white)
- Ensure width and height are non-zero

### Performance Issues
- Use clipping sparingly for maximum performance
- Batch operations when possible
- Consider object size and alignment

## Technical Details

### Hardware Architecture
- Word-level operations with pixel-precise masking
- Bresenham-style line generation for efficient memory access
- Automatic stride calculation for framebuffer addressing
- Pipelined state machine for maximum throughput

### Memory Interface
- 32-bit wide memory access via Wishbone protocol
- Burst-capable for large operations
- Automatic address generation and bounds checking
- Optimized for 1-bit-per-pixel framebuffer format

