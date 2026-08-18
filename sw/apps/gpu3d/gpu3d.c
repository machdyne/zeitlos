#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "../../common/zeitlos.h"
#include "../../common/zwm.h"
#include "../../common/zwin.h"

// register access, IRQ-masked atomicity, and clip-region management
// for the hardware line rasterizer are all handled by
// z_win_hw_line() (zwin.h) now -- this file never touches the
// gpu_*/gpu_clip_* registers directly. See zgfx.h's file header
// comment for why that hardware access needed to move behind a
// shared, careful implementation rather than each app managing it
// (badly, it turned out) on its own.

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
#define SCREEN_WIDTH 640
#define SCREEN_HEIGHT 480
// scaled down along with WIN_WIDTH/HEIGHT below -- was 300 for the
// original full-screen (512x384) version. Projected extent scales
// linearly with this value (z_offset is independent of it), so this
// was derived directly from the same numeric sweep referenced below:
// 300 gave a ~111px worst-case extent; 140 gives ~52px, verified
// against the smaller window's own content-area size.
#define PROJECTION_DISTANCE INT_TO_FIXED(140)

// window size requested from the wm -- similar footprint to
// hello_win (160x100) rather than the earlier 320x320, which turned
// out to be large enough that the wm's window placement (a fixed
// cascade with no check against window size -- see wm.c's
// create_window()) could push it partially off the 384px-tall
// screen. 160x160 (square, suited to a cube rather than text) fits
// safely regardless of cascade position. PROJECTION_DISTANCE above
// was rescaled to match -- see update_win_geometry()'s content-area
// math: worst-case projected extent is ~52px against a ~72-78px
// content-area half-extent here, comfortable margin either way.
#define WIN_WIDTH   160
#define WIN_HEIGHT  160

static z_win_t win;

// projection center -- was a fixed SCREEN_CENTER_X/Y compile-time
// constant before windowing (the screen's own center); now tracks
// the window's content area instead, recomputed by
// update_win_geometry() below whenever the window is created or
// moved.
static int win_center_x, win_center_y;

// content-area bounds, for centering the projection. z_win_hw_line()
// (zwin.h) computes and applies this same rectangle to the hardware
// clip registers itself, on every call -- this file only needs it
// for the projection math below, via the same z_win_content_rect()
// call zwin.c's own drawing functions use internally, rather than
// keeping a separate, possibly-drifting copy of the formula (which
// is exactly what happened before this -- see docs/window_manager.md).
static void update_win_geometry(void) {

	z_clip_t clip;
	z_win_content_rect(&win, &clip);

	win_center_x = (int)((clip.x0 + clip.x1) / 2);
	win_center_y = (int)((clip.y0 + clip.y1) / 2);

}

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
    result.x = win_center_x + FIXED_TO_INT(fixed_div(fixed_mul(v.x, PROJECTION_DISTANCE), z_offset));
    result.y = win_center_y - FIXED_TO_INT(fixed_div(fixed_mul(v.y, PROJECTION_DISTANCE), z_offset));
    
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

void gpu3d_uart_putc(char c) {
    uart_tx = c;
    delay();
}

void gpu3d_uart_puts(const char *s) {
    while (*s) {
        gpu3d_uart_putc(*s++);
    }
}

void gpu3d_uart_putnum(uint32_t n) {
    char buf[12];
    int i = 0;
    if (n == 0) {
        gpu3d_uart_putc('0');
        return;
    }
    while (n > 0 && i < 11) {
        buf[i++] = '0' + (n % 10);
        n /= 10;
    }
    while (i--) gpu3d_uart_putc(buf[i]);
}

// Draw line with timeout
void draw_line(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint8_t color) {
    // z_win_hw_line() (zwin.h) handles the FIFO-wait, clip-region
    // setup, and IRQ-masked atomicity -- kept as a thin wrapper under
    // this same name/signature so every existing call site in this
    // file (including the unused demo functions further down) stays
    // unchanged.
    z_win_hw_line(&win, x0, y0, x1, y1, color);
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
    gpu3d_uart_puts("Demo: Spinning Cube\r\n");
    
    for (int frame = 0; frame < frames; frame++) {
        int angle = (frame * 2) % 360;
			clear_screen();
        render_object(&cube, angle, (angle * 13) / 10, (angle * 7) / 10, 1);
        
  //      simple_delay(50000);  // Adjust delay for your system
    }
}

// Demo 2: Multiple objects cycling
void demo_morphing_objects(int frames) {
    gpu3d_uart_puts("Demo: Morphing Objects\r\n");
    
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
    gpu3d_uart_puts("Demo: Complex Animation\r\n");
    
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
    gpu3d_uart_puts("Testing basic line drawing...\r\n");
    
    
    // Draw some test lines
    draw_line(100, 100, 200, 100, 1); // Horizontal line
    draw_line(100, 120, 100, 220, 1); // Vertical line  
    draw_line(120, 120, 180, 180, 1); // Diagonal line
    
    // Draw a simple square
    draw_line(250, 150, 350, 150, 1); // Top
    draw_line(350, 150, 350, 250, 1); // Right
    draw_line(350, 250, 250, 250, 1); // Bottom
    draw_line(250, 250, 250, 150, 1); // Left
    
    gpu3d_uart_puts("Basic lines drawn\r\n");
}

// Test projection without rotation
void test_projection() {
    gpu3d_uart_puts("Testing projection...\r\n");
    
    vertex3d_t test_vertex = {INT_TO_FIXED(0), INT_TO_FIXED(0), INT_TO_FIXED(0)};
    vertex2d_t projected = project_vertex(test_vertex);
    
    gpu3d_uart_puts("Center point projects to: ");
    gpu3d_uart_putnum(projected.x);
    gpu3d_uart_puts(", ");
    gpu3d_uart_putnum(projected.y);
    gpu3d_uart_puts("\r\n");
    
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
    printf("Starting FAST spinning cube demo...\r\n");

    int angle_x = 0, angle_y = 0, angle_z = 0;
    vertex2d_t prev_projected[8];
    int first_frame = 1;
    bool redraw_pending = false;	// a Z_WM_REDRAW arrived; call
					// z_win_redraw_done() once this
					// frame finishes drawing (see
					// docs/window_manager.md, "content
					// z-order")

    while (1) {

	// non-blocking message drain, same shape as hello_win's
	// drain_messages(). Z_WM_REDRAW means the wm just did a
	// full-screen clear (per zwm.h's own comment on that subject),
	// so there's nothing of ours left on screen to erase --
	// first_frame=1 skips straight to drawing a fresh frame at the
	// (possibly new) window position, reusing the same mechanism
	// that already exists below for the very first frame ever
	// drawn. Z_WM_WINDOW_MOVED updates window position/geometry the
	// same way but is deliberately not its own redraw trigger --
	// same reasoning as hello_win's drain_messages().
	z_msg_t msg;
	while (z_msg_read(&msg) == Z_OK) {
		if (msg.subject == Z_WM_REDRAW) {
			z_win_apply_redraw(&win, msg.obj.val.uint32);
			update_win_geometry();
			first_frame = 1;
			redraw_pending = true;
		} else if (msg.subject == Z_WM_WINDOW_MOVED) {
			z_win_parse_rect(&win, &msg.obj);
			update_win_geometry();
		}
	}

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

	if (redraw_pending) {
		z_win_redraw_done(&win);
		redraw_pending = false;
	}
        
        for (int i = 0; i < 8; i++) {
            prev_projected[i] = curr_projected[i];
        }
        
        first_frame = 0;
        
        // MUCH faster rotation - big angle steps
        angle_x = (angle_x + 5) % 360;  // 10 degree steps
        angle_y = (angle_y + 7) % 360;  // 15 degree steps
        angle_z = (angle_z + 3) % 360;   // 8 degree steps

    }
}

// Main function - choose which demo to run
int main() {
    printf("3D Spinning Object Demo - Fixed Point Version\r\n");
    printf("Using hardware line rasterizer\r\n");

    if (z_win_create(&win, "gpu3d", WIN_WIDTH, WIN_HEIGHT) != Z_OK) {
        printf("gpu3d: failed to create window\r\n");
        return 1;
    }

    update_win_geometry();
    z_win_clear(&win);

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
