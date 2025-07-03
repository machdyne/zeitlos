# Zeitlos GPU Line Rasterizer Developer Guide

## Overview

The Zeitlos GPU Line Rasterizer is a hardware-accelerated line drawing engine with built-in command FIFO buffering and clipping support. It uses the Bresenham line algorithm for pixel-perfect line rendering on a 512×384 monochrome (1-bit-per-pixel) framebuffer.

## Key Features

- **Hardware Bresenham line algorithm**
- **16-command deep FIFO buffer**
- **Configurable clipping rectangle**
- **Pixel-perfect line rendering**
- **Automatic anti-aliasing support**
- **High-performance batch processing**
- **Safe concurrent operation**

## Memory Map

| Address | Register | Description |
|---------|----------|-------------|
| 0xE0000000 | LINE_X0 | Line start X coordinate |
| 0xE0000004 | LINE_Y0 | Line start Y coordinate |
| 0xE0000008 | LINE_X1 | Line end X coordinate |
| 0xE000000C | LINE_Y1 | Line end Y coordinate |
| 0xE0000010 | LINE_COLOR | Line color (0=black, 1=white) |
| 0xE0000014 | LINE_START | Start line drawing (write-only) |
| 0xE0000018 | LINE_STATUS | Status register (read-only) |
| 0xE000001C | LINE_PIXEL_COUNT | Current pixel count (read-only) |
| 0xE0000020 | LINE_CUR_X | Current X position (read-only) |
| 0xE0000024 | LINE_CUR_Y | Current Y position (read-only) |
| 0xE0000028 | LINE_FIFO_COUNT | FIFO depth (read-only) |
| 0xE000002C | CLIP_X0 | Clip rectangle left |
| 0xE0000030 | CLIP_Y0 | Clip rectangle top |
| 0xE0000034 | CLIP_X1 | Clip rectangle right |
| 0xE0000038 | CLIP_Y1 | Clip rectangle bottom |
| 0xE000003C | CLIP_ENABLE | Clipping enable (0=off, 1=on) |

## Status Register (LINE_STATUS)

| Bit | Name | Description |
|-----|------|-------------|
| 0 | BUSY | Rasterizer busy (1=busy, 0=idle) |

## Screen Specifications

- **Resolution**: 512×384 pixels
- **Color depth**: 1-bit per pixel (monochrome)
- **Framebuffer**: Located at 0x20000000
- **Coordinate system**: (0,0) at top-left, (511,383) at bottom-right

## Programming Interface

### C Header Definition

```c
#define RASTERIZER_BASE 0xE0000000

#define LINE_X0         (*(volatile uint32_t*)(RASTERIZER_BASE + 0x00))
#define LINE_Y0         (*(volatile uint32_t*)(RASTERIZER_BASE + 0x04))
#define LINE_X1         (*(volatile uint32_t*)(RASTERIZER_BASE + 0x08))
#define LINE_Y1         (*(volatile uint32_t*)(RASTERIZER_BASE + 0x0C))
#define LINE_COLOR      (*(volatile uint32_t*)(RASTERIZER_BASE + 0x10))
#define LINE_START      (*(volatile uint32_t*)(RASTERIZER_BASE + 0x14))
#define LINE_STATUS     (*(volatile uint32_t*)(RASTERIZER_BASE + 0x18))
#define LINE_PIXEL_COUNT (*(volatile uint32_t*)(RASTERIZER_BASE + 0x1C))
#define LINE_CUR_X      (*(volatile uint32_t*)(RASTERIZER_BASE + 0x20))
#define LINE_CUR_Y      (*(volatile uint32_t*)(RASTERIZER_BASE + 0x24))
#define LINE_FIFO_COUNT (*(volatile uint32_t*)(RASTERIZER_BASE + 0x28))
#define CLIP_X0         (*(volatile uint32_t*)(RASTERIZER_BASE + 0x2C))
#define CLIP_Y0         (*(volatile uint32_t*)(RASTERIZER_BASE + 0x30))
#define CLIP_X1         (*(volatile uint32_t*)(RASTERIZER_BASE + 0x34))
#define CLIP_Y1         (*(volatile uint32_t*)(RASTERIZER_BASE + 0x38))
#define CLIP_ENABLE     (*(volatile uint32_t*)(RASTERIZER_BASE + 0x3C))

#define COLOR_BLACK     0
#define COLOR_WHITE     1
```

### Basic Usage

#### 1. Check FIFO Status

The rasterizer has a 16-command deep FIFO. Check space before queuing commands:

```c
int fifo_space_available(void) {
    return 16 - LINE_FIFO_COUNT;
}

void wait_for_fifo_space(void) {
    while (LINE_FIFO_COUNT >= 16) {
        // Wait for FIFO space
    }
}
```

#### 2. Draw a Line

```c
void draw_line(int x0, int y0, int x1, int y1, int color) {
    // Wait for FIFO space
    wait_for_fifo_space();
    
    // Set line parameters
    LINE_X0 = x0;
    LINE_Y0 = y0;
    LINE_X1 = x1;
    LINE_Y1 = y1;
    LINE_COLOR = color;
    
    // Start line drawing (queues command in FIFO)
    LINE_START = 1;
}
```

#### 3. Wait for Completion

```c
void wait_lines_complete(void) {
    while (LINE_STATUS & 1) {
        // Wait for all lines to complete
    }
}
```

## Clipping Support

### Configure Clipping Rectangle

```c
void set_clip_rect(int x0, int y0, int x1, int y1) {
    CLIP_X0 = x0;
    CLIP_Y0 = y0;
    CLIP_X1 = x1;
    CLIP_Y1 = y1;
    CLIP_ENABLE = 1;
}

void disable_clipping(void) {
    CLIP_ENABLE = 0;
}
```

### Full Screen Clipping

```c
void set_full_screen_clip(void) {
    set_clip_rect(0, 0, 511, 383);
}
```

## Common Use Cases

### Drawing Basic Shapes

#### Rectangle Outline
```c
void draw_rect(int x, int y, int width, int height, int color) {
    draw_line(x, y, x + width - 1, y, color);              // Top
    draw_line(x + width - 1, y, x + width - 1, y + height - 1, color); // Right
    draw_line(x + width - 1, y + height - 1, x, y + height - 1, color); // Bottom
    draw_line(x, y + height - 1, x, y, color);             // Left
}
```

#### Triangle
```c
void draw_triangle(int x0, int y0, int x1, int y1, int x2, int y2, int color) {
    draw_line(x0, y0, x1, y1, color);
    draw_line(x1, y1, x2, y2, color);
    draw_line(x2, y2, x0, y0, color);
}
```

#### Circle (Octagon Approximation)
```c
void draw_circle(int cx, int cy, int radius, int color) {
    int points = 8;
    int prev_x = cx + radius;
    int prev_y = cy;
    
    for (int i = 1; i <= points; i++) {
        int angle = (i * 360) / points;
        int x = cx + (radius * cos_table[angle]) / 256;
        int y = cy + (radius * sin_table[angle]) / 256;
        draw_line(prev_x, prev_y, x, y, color);
        prev_x = x;
        prev_y = y;
    }
}
```

### Batch Line Drawing

```c
void draw_line_batch(struct line_cmd *lines, int count) {
    for (int i = 0; i < count; i++) {
        // Wait for FIFO space if needed
        if (LINE_FIFO_COUNT >= 15) {
            wait_for_fifo_space();
        }
        
        draw_line(lines[i].x0, lines[i].y0, 
                 lines[i].x1, lines[i].y1, 
                 lines[i].color);
    }
}
```

### Vector Graphics

```c
void draw_wireframe_cube(int cx, int cy, int size, int color) {
    int half = size / 2;
    
    // Front face
    draw_rect(cx - half, cy - half, size, size, color);
    
    // Back face (offset for 3D effect)
    int offset = size / 4;
    draw_rect(cx - half + offset, cy - half - offset, size, size, color);
    
    // Connecting lines
    draw_line(cx - half, cy - half, cx - half + offset, cy - half - offset, color);
    draw_line(cx + half, cy - half, cx + half + offset, cy - half - offset, color);
    draw_line(cx - half, cy + half, cx - half + offset, cy + half - offset, color);
    draw_line(cx + half, cy + half, cx + half + offset, cy + half - offset, color);
}
```

## Performance Characteristics

### FIFO Benefits

- **Concurrent operation**: CPU can queue while GPU draws
- **Batch processing**: Submit multiple lines without waiting
- **Smooth animation**: Consistent frame rates
- **Reduced latency**: Hardware buffering eliminates stalls

### Performance Metrics

| Operation | Cycles per Line | Throughput |
|-----------|----------------|------------|
| Short lines (1-10 pixels) | 50-100 | Very High |
| Medium lines (10-50 pixels) | 100-500 | High |
| Long lines (50+ pixels) | 500+ | Good |
| Diagonal lines | Variable | Depends on length |

### Optimization Tips

1. **Batch operations**: Use FIFO efficiently
2. **Minimize clipping**: Disable when not needed
3. **Order matters**: Draw back-to-front for complex scenes
4. **Use appropriate coordinates**: Stay within screen bounds

## Advanced Features

### Real-time Line Monitoring

```c
void monitor_line_progress(void) {
    printf("Current position: (%d, %d)\n", LINE_CUR_X, LINE_CUR_Y);
    printf("Pixels drawn: %d\n", LINE_PIXEL_COUNT);
    printf("FIFO depth: %d\n", LINE_FIFO_COUNT);
}
```

### Clipped Line Drawing

```c
void draw_clipped_line(int x0, int y0, int x1, int y1, int color, 
                      int clip_x0, int clip_y0, int clip_x1, int clip_y1) {
    // Set clipping rectangle
    set_clip_rect(clip_x0, clip_y0, clip_x1, clip_y1);
    
    // Draw line (automatically clipped)
    draw_line(x0, y0, x1, y1, color);
    
    // Restore full screen clipping
    set_full_screen_clip();
}
```

### Animation Support

```c
void animate_rotating_line(int cx, int cy, int radius) {
    for (int angle = 0; angle < 360; angle += 5) {
        // Clear previous line (draw in black)
        if (angle > 0) {
            int prev_x = cx + (radius * cos_table[angle - 5]) / 256;
            int prev_y = cy + (radius * sin_table[angle - 5]) / 256;
            draw_line(cx, cy, prev_x, prev_y, COLOR_BLACK);
        }
        
        // Draw new line (draw in white)
        int x = cx + (radius * cos_table[angle]) / 256;
        int y = cy + (radius * sin_table[angle]) / 256;
        draw_line(cx, cy, x, y, COLOR_WHITE);
        
        // Wait for completion
        wait_lines_complete();
        
        delay(50);
    }
}
```

## Error Handling and Debugging

### FIFO Overflow Protection

```c
int safe_draw_line(int x0, int y0, int x1, int y1, int color) {
    if (LINE_FIFO_COUNT >= 16) {
        return 0;  // FIFO full, cannot queue
    }
    
    draw_line(x0, y0, x1, y1, color);
    return 1;  // Success
}
```

### Coordinate Validation

```c
int validate_coordinates(int x, int y) {
    return (x >= 0 && x < 512 && y >= 0 && y < 384);
}

void draw_safe_line(int x0, int y0, int x1, int y1, int color) {
    if (!validate_coordinates(x0, y0) || !validate_coordinates(x1, y1)) {
        printf("Warning: Line coordinates out of bounds\n");
        return;
    }
    
    draw_line(x0, y0, x1, y1, color);
}
```

## Best Practices

### 1. FIFO Management
```c
// Good: Check FIFO space before batch operations
void draw_polygon(int *points, int count, int color) {
    for (int i = 0; i < count; i++) {
        if (LINE_FIFO_COUNT >= 15) {
            wait_for_fifo_space();
        }
        
        int next = (i + 1) % count;
        draw_line(points[i*2], points[i*2+1], 
                 points[next*2], points[next*2+1], color);
    }
}
```

### 2. Clipping Optimization
```c
// Disable clipping for known-safe operations
void draw_crosshair(int cx, int cy, int size, int color) {
    disable_clipping();
    
    draw_line(cx - size, cy, cx + size, cy, color);
    draw_line(cx, cy - size, cx, cy + size, color);
    
    set_full_screen_clip();  // Re-enable for safety
}
```

### 3. Performance Monitoring
```c
void performance_test(void) {
    uint32_t start_time = get_time();
    
    // Draw 1000 random lines
    for (int i = 0; i < 1000; i++) {
        draw_line(rand() % 512, rand() % 384, 
                 rand() % 512, rand() % 384, COLOR_WHITE);
    }
    
    wait_lines_complete();
    uint32_t end_time = get_time();
    
    printf("Drew 1000 lines in %d ms\n", end_time - start_time);
}
```

## Troubleshooting

### Lines Not Appearing
- Check FIFO space before drawing
- Verify coordinates are within screen bounds
- Ensure clipping rectangle is properly configured
- Confirm color value (0 or 1)

### Performance Issues
- Monitor FIFO depth with `LINE_FIFO_COUNT`
- Avoid blocking on full FIFO
- Use batch operations efficiently
- Consider disabling clipping for simple scenes

### Clipping Problems
- Verify clipping rectangle coordinates
- Check `CLIP_ENABLE` status
- Ensure clipping bounds are reasonable
- Test with clipping disabled

## Hardware Details

### Bresenham Algorithm
The rasterizer implements the classic Bresenham line algorithm in hardware:
- Pixel-perfect line rendering
- No floating-point calculations
- Efficient integer arithmetic
- Optimized for all line orientations

### FIFO Architecture
- 16-command deep buffer
- Automatic push/pop control
- Concurrent CPU/GPU operation
- Overflow protection

### Memory Interface
- Direct framebuffer access
- Read-modify-write for pixel updates
- Optimized for 1-bit-per-pixel format
- Automatic word-level operations

