#include <stdio.h>
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

// Your hardware line rasterizer function
void draw_line(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint8_t color);
void clear_screen(void);

// Fixed-point math configuration
#define FIXED_SHIFT 12
#define FIXED_ONE (1 << FIXED_SHIFT)
#define FIXED_HALF (FIXED_ONE >> 1)

// Fixed-point type
typedef int32_t fixed_t;

// Convert between fixed and integer
#define INT_TO_FIXED(x) ((fixed_t)((x) << FIXED_SHIFT))
#define FIXED_TO_INT(x) ((int)((x) >> FIXED_SHIFT))
#define FLOAT_TO_FIXED(x) ((fixed_t)((x) * FIXED_ONE))

// Fixed-point 3D vertex structure
typedef struct {
    fixed_t x, y, z;
} vertex3d_t;

// 2D vertex structure  
typedef struct {
    int x, y;
} vertex2d_t;

// Edge structure to define connections between vertices
typedef struct {
    int v0, v1;  // indices of vertices to connect
} edge_t;

// 3D object structure
typedef struct {
    vertex3d_t *vertices;
    edge_t *edges;
    int num_vertices;
    int num_edges;
} object3d_t;

// Screen configuration (adjust to match your display)
#define SCREEN_WIDTH 512
#define SCREEN_HEIGHT 384
#define SCREEN_CENTER_X (SCREEN_WIDTH/2)
#define SCREEN_CENTER_Y (SCREEN_HEIGHT/2)
#define PROJECTION_DISTANCE INT_TO_FIXED(300)

// ============================================================================
// FIXED-POINT MATH FUNCTIONS
// ============================================================================

// Fixed-point multiplication
fixed_t fixed_mul(fixed_t a, fixed_t b) {
    return (fixed_t)(((int64_t)a * b) >> FIXED_SHIFT);
}

// Fixed-point division
fixed_t fixed_div(fixed_t a, fixed_t b) {
    return (fixed_t)(((int64_t)a << FIXED_SHIFT) / b);
}

// Simple sine lookup table (90 entries for 0-89 degrees)
static const fixed_t sin_table[90] = {
    0, 71, 143, 214, 285, 357, 428, 499, 570, 641,
    711, 781, 851, 921, 990, 1060, 1128, 1197, 1265, 1333,
    1400, 1468, 1534, 1600, 1665, 1730, 1795, 1859, 1922, 1985,
    2048, 2109, 2170, 2230, 2290, 2349, 2407, 2464, 2521, 2577,
    2632, 2687, 2741, 2794, 2846, 2897, 2948, 2998, 3047, 3095,
    3142, 3189, 3234, 3279, 3322, 3365, 3406, 3447, 3486, 3525,
    3562, 3598, 3633, 3668, 3701, 3733, 3764, 3794, 3823, 3851,
    3878, 3904, 3929, 3952, 3975, 3996, 4017, 4036, 4054, 4071,
    4087, 4102, 4116, 4129, 4140, 4151, 4160, 4169, 4176, 4182
};

// Fast sine function using lookup table
fixed_t fixed_sin(int angle_deg) {
    angle_deg = angle_deg % 360;
    if (angle_deg < 0) angle_deg += 360;
    
    if (angle_deg < 90) {
        return sin_table[angle_deg];
    } else if (angle_deg < 180) {
        return sin_table[179 - angle_deg];
    } else if (angle_deg < 270) {
        return -sin_table[angle_deg - 180];
    } else {
        return -sin_table[359 - angle_deg];
    }
}

// Fast cosine function
fixed_t fixed_cos(int angle_deg) {
    return fixed_sin(angle_deg + 90);
}

// ============================================================================
// 3D OBJECTS DEFINITIONS
// ============================================================================

// Cube vertices (using fixed-point)
vertex3d_t cube_vertices[8] = {
    {INT_TO_FIXED(-1), INT_TO_FIXED(-1), INT_TO_FIXED(-1)},  // 0: back-bottom-left
    {INT_TO_FIXED( 1), INT_TO_FIXED(-1), INT_TO_FIXED(-1)},  // 1: back-bottom-right
    {INT_TO_FIXED( 1), INT_TO_FIXED( 1), INT_TO_FIXED(-1)},  // 2: back-top-right
    {INT_TO_FIXED(-1), INT_TO_FIXED( 1), INT_TO_FIXED(-1)},  // 3: back-top-left
    {INT_TO_FIXED(-1), INT_TO_FIXED(-1), INT_TO_FIXED( 1)},  // 4: front-bottom-left
    {INT_TO_FIXED( 1), INT_TO_FIXED(-1), INT_TO_FIXED( 1)},  // 5: front-bottom-right
    {INT_TO_FIXED( 1), INT_TO_FIXED( 1), INT_TO_FIXED( 1)},  // 6: front-top-right
    {INT_TO_FIXED(-1), INT_TO_FIXED( 1), INT_TO_FIXED( 1)}   // 7: front-top-left
};

// Cube edges (12 edges total)
edge_t cube_edges[12] = {
    {0, 1}, {1, 2}, {2, 3}, {3, 0},  // back face
    {4, 5}, {5, 6}, {6, 7}, {7, 4},  // front face
    {0, 4}, {1, 5}, {2, 6}, {3, 7}   // connecting edges
};

// Tetrahedron vertices
vertex3d_t tetra_vertices[4] = {
    {INT_TO_FIXED( 0), FLOAT_TO_FIXED( 1.5), INT_TO_FIXED( 0)},   // top
    {INT_TO_FIXED(-1), FLOAT_TO_FIXED(-0.5), INT_TO_FIXED(-1)},   // back-left
    {INT_TO_FIXED( 1), FLOAT_TO_FIXED(-0.5), INT_TO_FIXED(-1)},   // back-right
    {INT_TO_FIXED( 0), FLOAT_TO_FIXED(-0.5), INT_TO_FIXED( 1)}    // front
};

// Tetrahedron edges (6 edges total)
edge_t tetra_edges[6] = {
    {0, 1}, {0, 2}, {0, 3},  // from top to base
    {1, 2}, {2, 3}, {3, 1}   // base triangle
};

// Octahedron vertices
vertex3d_t octa_vertices[6] = {
    {INT_TO_FIXED( 0), FLOAT_TO_FIXED( 1.5), INT_TO_FIXED( 0)},   // top
    {INT_TO_FIXED( 0), FLOAT_TO_FIXED(-1.5), INT_TO_FIXED( 0)},   // bottom
    {FLOAT_TO_FIXED( 1.5), INT_TO_FIXED( 0), INT_TO_FIXED( 0)},   // right
    {FLOAT_TO_FIXED(-1.5), INT_TO_FIXED( 0), INT_TO_FIXED( 0)},   // left
    {INT_TO_FIXED( 0), INT_TO_FIXED( 0), FLOAT_TO_FIXED( 1.5)},   // front
    {INT_TO_FIXED( 0), INT_TO_FIXED( 0), FLOAT_TO_FIXED(-1.5)}    // back
};

// Octahedron edges (12 edges total)
edge_t octa_edges[12] = {
    {0, 2}, {0, 3}, {0, 4}, {0, 5},  // top to sides
    {1, 2}, {1, 3}, {1, 4}, {1, 5},  // bottom to sides
    {2, 4}, {4, 3}, {3, 5}, {5, 2}   // around middle
};

// Create object definitions
object3d_t cube = {cube_vertices, cube_edges, 8, 12};
object3d_t tetrahedron = {tetra_vertices, tetra_edges, 4, 6};
object3d_t octahedron = {octa_vertices, octa_edges, 6, 12};

// ============================================================================
// 3D MATH FUNCTIONS
// ============================================================================

// Rotate a vertex around X axis
vertex3d_t rotate_x(vertex3d_t v, int angle_deg) {
    fixed_t cos_a = fixed_cos(angle_deg);
    fixed_t sin_a = fixed_sin(angle_deg);
    vertex3d_t result;
    result.x = v.x;
    result.y = fixed_mul(v.y, cos_a) - fixed_mul(v.z, sin_a);
    result.z = fixed_mul(v.y, sin_a) + fixed_mul(v.z, cos_a);
    return result;
}

// Rotate a vertex around Y axis
vertex3d_t rotate_y(vertex3d_t v, int angle_deg) {
    fixed_t cos_a = fixed_cos(angle_deg);
    fixed_t sin_a = fixed_sin(angle_deg);
    vertex3d_t result;
    result.x = fixed_mul(v.x, cos_a) + fixed_mul(v.z, sin_a);
    result.y = v.y;
    result.z = -fixed_mul(v.x, sin_a) + fixed_mul(v.z, cos_a);
    return result;
}

// Rotate a vertex around Z axis
vertex3d_t rotate_z(vertex3d_t v, int angle_deg) {
    fixed_t cos_a = fixed_cos(angle_deg);
    fixed_t sin_a = fixed_sin(angle_deg);
    vertex3d_t result;
    result.x = fixed_mul(v.x, cos_a) - fixed_mul(v.y, sin_a);
    result.y = fixed_mul(v.x, sin_a) + fixed_mul(v.y, cos_a);
    result.z = v.z;
    return result;
}

// Apply all three rotations to a vertex
vertex3d_t rotate_vertex(vertex3d_t v, int rx, int ry, int rz) {
    v = rotate_x(v, rx);
    v = rotate_y(v, ry);
    v = rotate_z(v, rz);
    return v;
}

// Project 3D vertex to 2D screen coordinates using perspective projection
vertex2d_t project_vertex(vertex3d_t v) {
    vertex2d_t result;
    
    // Move object away from camera to avoid clipping
    fixed_t z_offset = v.z + INT_TO_FIXED(5);
    
    // Avoid division by zero or negative z
    if (z_offset <= FLOAT_TO_FIXED(0.1)) z_offset = FLOAT_TO_FIXED(0.1);
    
    // Perspective projection
    result.x = SCREEN_CENTER_X + FIXED_TO_INT(fixed_div(fixed_mul(v.x, PROJECTION_DISTANCE), z_offset));
    result.y = SCREEN_CENTER_Y - FIXED_TO_INT(fixed_div(fixed_mul(v.y, PROJECTION_DISTANCE), z_offset));
    
    return result;
}

// Check if a 2D point is within screen bounds
int is_on_screen(vertex2d_t v) {
    return (v.x >= 0 && v.x < SCREEN_WIDTH && v.y >= 0 && v.y < SCREEN_HEIGHT);
}

// ============================================================================
// RENDERING FUNCTIONS
// ============================================================================

// Render a 3D object as wireframe
void render_object(object3d_t *obj, int rx, int ry, int rz, uint8_t color) {
    // Transform and project all vertices
    vertex2d_t projected[16];  // Max vertices we expect
    
    for (int i = 0; i < obj->num_vertices; i++) {
        vertex3d_t transformed = rotate_vertex(obj->vertices[i], rx, ry, rz);
        projected[i] = project_vertex(transformed);
    }
    
    // Draw all edges
    for (int i = 0; i < obj->num_edges; i++) {
        edge_t edge = obj->edges[i];
        vertex2d_t v0 = projected[edge.v0];
        vertex2d_t v1 = projected[edge.v1];
        
        // Simple clipping - only draw if both points are on screen
        if (is_on_screen(v0) && is_on_screen(v1)) {
            draw_line(v0.x, v0.y, v1.x, v1.y, color);
        }
    }
}

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

    while (gpu_debug_fifo_count > 15) /* wait */;

    gpu_x0 = x0;
    gpu_x0 = x0;
    gpu_y0 = y0;
    gpu_x1 = x1;
    gpu_y1 = y1;
    gpu_color = color & 1;
    gpu_clip_enable = 0;
    gpu_start = 1;

}


// ============================================================================
// DEMO FUNCTIONS
// ============================================================================

// Simple delay function (adjust for your system)
void simple_delay(int cycles) {
    volatile int i;
    for (i = 0; i < cycles; i++) {
        // Do nothing, just burn cycles
    }
}

// Demo 1: Single spinning cube
void demo_spinning_cube(int frames) {
    uart_puts("Demo: Spinning Cube\r\n");
    
    for (int frame = 0; frame < frames; frame++) {
        int angle = (frame * 2) % 360;
			clear_screen();
        render_object(&cube, angle, (angle * 13) / 10, (angle * 7) / 10, 1);
        
  //      simple_delay(50000);  // Adjust delay for your system
    }
}

// Demo 2: Multiple objects cycling
void demo_morphing_objects(int frames) {
    uart_puts("Demo: Morphing Objects\r\n");
    
    object3d_t *objects[] = {&cube, &tetrahedron, &octahedron};
    int num_objects = 3;
    int frames_per_object = frames / num_objects;
    
    for (int frame = 0; frame < frames; frame++) {
        int object_index = frame / frames_per_object;
        if (object_index >= num_objects) object_index = num_objects - 1;
        
        int angle = (frame * 3) % 360;
			clear_screen();
        render_object(objects[object_index], angle, (angle * 12) / 10, (angle * 8) / 10, 1);
        
      //  simple_delay(75000);
    }
}

// Demo 3: Complex animation with varying speeds
void demo_complex_animation(int frames) {
    uart_puts("Demo: Complex Animation\r\n");
    
    for (int frame = 0; frame < frames; frame++) {
        // Use different angle calculations for organic movement
        int base_angle = (frame * 2) % 360;
        int rx = (FIXED_TO_INT(fixed_sin(base_angle * 2)) >> 1) + base_angle;  // Oscillating + rotating
        int ry = base_angle * 15 / 10;  // Faster Y rotation
        int rz = (FIXED_TO_INT(fixed_cos(base_angle * 13 / 10)) >> 2) + (base_angle * 3 / 10);  // Complex Z
        
			clear_screen();
        render_object(&octahedron, rx % 360, ry % 360, rz % 360, 1);
        
     //   simple_delay(50000);
    }
}

// ============================================================================
// DEBUG AND TEST FUNCTIONS
// ============================================================================

// Test basic line drawing
void test_basic_lines() {
    uart_puts("Testing basic line drawing...\r\n");
    
    
    // Draw some test lines
    draw_line(100, 100, 200, 100, 1); // Horizontal line
    draw_line(100, 120, 100, 220, 1); // Vertical line  
    draw_line(120, 120, 180, 180, 1); // Diagonal line
    
    // Draw a simple square
    draw_line(250, 150, 350, 150, 1); // Top
    draw_line(350, 150, 350, 250, 1); // Right
    draw_line(350, 250, 250, 250, 1); // Bottom
    draw_line(250, 250, 250, 150, 1); // Left
    
    uart_puts("Basic lines drawn\r\n");
}

// Test projection without rotation
void test_projection() {
    uart_puts("Testing projection...\r\n");
    
    vertex3d_t test_vertex = {INT_TO_FIXED(0), INT_TO_FIXED(0), INT_TO_FIXED(0)};
    vertex2d_t projected = project_vertex(test_vertex);
    
    uart_puts("Center point projects to: ");
    uart_putnum(projected.x);
    uart_puts(", ");
    uart_putnum(projected.y);
    uart_puts("\r\n");
    
    // Draw a point at the projected center
    draw_line(projected.x, projected.y, projected.x, projected.y, 1);
}

void clear_screen(void) {
	for (int y = 0; y < SCREEN_HEIGHT; y += 2) {
		draw_line(0, y, SCREEN_WIDTH - 1, y, 0);     // Even lines
		draw_line(0, y + 1, SCREEN_WIDTH - 1, y + 1, 0); // Odd lines
	}
}

// ============================================================================
// MAIN DEMO INTERFACE
// ============================================================================

// Run all demos
void run_3d_demos() {
    // Run each demo for a certain number of frames
    demo_spinning_cube(200);
    clear_screen();
    demo_morphing_objects(300);
    clear_screen();
    demo_complex_animation(250);
}

// Ultra-fast cube with bigger angle steps
void infinite_spinning_cube() {
    uart_puts("Starting FAST spinning cube demo...\r\n");
    
    gpu_clip_enable = 0;
    
    int angle_x = 0, angle_y = 0, angle_z = 0;
    vertex2d_t prev_projected[8];
    int first_frame = 1;
    
    while (1) {
        vertex2d_t curr_projected[8];
        for (int i = 0; i < 8; i++) {
            vertex3d_t transformed = rotate_vertex(cube_vertices[i], angle_x, angle_y, angle_z);
            curr_projected[i] = project_vertex(transformed);
        }
        
        if (!first_frame) {
            for (int i = 0; i < 12; i++) {
                edge_t edge = cube_edges[i];
                vertex2d_t v0 = prev_projected[edge.v0];
                vertex2d_t v1 = prev_projected[edge.v1];
                
                if (is_on_screen(v0) && is_on_screen(v1)) {
                    draw_line(v0.x, v0.y, v1.x, v1.y, 0);
                }
            }
        }
        
        for (int i = 0; i < 12; i++) {
            edge_t edge = cube_edges[i];
            vertex2d_t v0 = curr_projected[edge.v0];
            vertex2d_t v1 = curr_projected[edge.v1];
            
            if (is_on_screen(v0) && is_on_screen(v1)) {
                draw_line(v0.x, v0.y, v1.x, v1.y, 1);
            }
        }
        
        for (int i = 0; i < 8; i++) {
            prev_projected[i] = curr_projected[i];
        }
        
        first_frame = 0;
        
        // MUCH faster rotation - big angle steps
        angle_x = (angle_x + 10) % 360;  // 10 degree steps
        angle_y = (angle_y + 15) % 360;  // 15 degree steps
        angle_z = (angle_z + 8) % 360;   // 8 degree steps
    }
}

// Main function - choose which demo to run
int main() {
    uart_puts("3D Spinning Object Demo - Fixed Point Version\r\n");
    uart_puts("Using hardware line rasterizer\r\n");

		clear_screen();
   

//test_basic_lines();
 
    // Option 1: Run finite demos
 //   run_3d_demos();
    
    // Option 2: Run infinite spinning cube (uncomment to use)
    infinite_spinning_cube();
    
    return 0;
}

// ============================================================================
// INTEGRATION NOTES:
// ============================================================================
/*
 * This version uses:
 * - Fixed-point arithmetic instead of floating-point
 * - Integer sine/cosine lookup tables
 * - No external math library dependencies
 * - Simple delay function (replace with your timer)
 * 
 * To compile: gcc -nostartfiles -o gpu3d.elf crt0.o gpu3d.o
 * (No -lm needed!)
 * 
 * Performance optimizations:
 * - All math is integer-based
 * - Lookup tables for trig functions
 * - Fixed-point preserves precision
 * - Minimal memory usage
 */
