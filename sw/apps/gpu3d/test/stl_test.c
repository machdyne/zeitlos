/* Host-side test for gpu3d's STL loader. Not part of the app. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "model.h"
#include "stl.h"

static model_t m;

static int pumps = 0;
static void pump(void) { pumps++; }

/* ---- tiny ASCII-art wireframe renderer, to eyeball the result ---- */

#define AW 78
#define AH 34
static char fb[AH][AW + 1];

static void plot(int x, int y) {
	if (x < 0 || x >= AW || y < 0 || y >= AH) return;
	fb[y][x] = '#';
}

static void line(int x0, int y0, int x1, int y1) {
	int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
	int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
	int err = dx + dy;
	for (;;) {
		plot(x0, y0);
		if (x0 == x1 && y0 == y1) break;
		int e2 = 2 * err;
		if (e2 >= dy) { err += dy; x0 += sx; }
		if (e2 <= dx) { err += dx; y0 += sy; }
	}
}

static void render(double ry) {
	for (int y = 0; y < AH; y++) { memset(fb[y], ' ', AW); fb[y][AW] = 0; }
	static int px[MODEL_MAX_VERTS], py[MODEL_MAX_VERTS];
	for (int i = 0; i < m.nverts; i++) {
		double x = m.verts[i].x / (double)FIXED_ONE;
		double y = m.verts[i].y / (double)FIXED_ONE;
		double z = m.verts[i].z / (double)FIXED_ONE;
		double xr = x * cos(ry) + z * sin(ry);
		double zr = -x * sin(ry) + z * cos(ry);
		double zo = zr + 5.0;
		if (zo < 0.1) zo = 0.1;
		px[i] = (int)(AW / 2 + (xr * 3.4 / zo) * AW / 2.2);
		py[i] = (int)(AH / 2 - (y * 3.4 / zo) * AH / 2.2);
	}
	for (int e = 0; e < m.nedges; e++)
		line(px[m.edges[e].v0], py[m.edges[e].v0],
		     px[m.edges[e].v1], py[m.edges[e].v1]);
	for (int y = 0; y < AH; y++) printf("|%s|\n", fb[y]);
}

static void check_extent(void) {
	fixed_t mn[3] = { INT32_MAX, INT32_MAX, INT32_MAX };
	fixed_t mx[3] = { INT32_MIN, INT32_MIN, INT32_MIN };
	for (int i = 0; i < m.nverts; i++) {
		fixed_t c[3] = { m.verts[i].x, m.verts[i].y, m.verts[i].z };
		for (int a = 0; a < 3; a++) {
			if (c[a] < mn[a]) mn[a] = c[a];
			if (c[a] > mx[a]) mx[a] = c[a];
		}
	}
	double half = 0;
	printf("  bbox:");
	for (int a = 0; a < 3; a++) {
		double lo = mn[a] / (double)FIXED_ONE, hi = mx[a] / (double)FIXED_ONE;
		printf(" [%+.3f,%+.3f]", lo, hi);
		if ((hi - lo) / 2 > half) half = (hi - lo) / 2;
	}
	printf("\n  largest half-extent: %.4f (must be ~1.000)\n", half);
	if (fabs(half - 1.0) > 0.02) { printf("  *** FAIL: not normalized\n"); exit(1); }
	/* centred? */
	for (int a = 0; a < 3; a++) {
		double mid = (mn[a] + mx[a]) / 2.0 / FIXED_ONE;
		if (fabs(mid) > 0.02) { printf("  *** FAIL: axis %d not centred (%.3f)\n", a, mid); exit(1); }
	}
	/* edge indices in range, no self-edges, no duplicates */
	for (int e = 0; e < m.nedges; e++) {
		if (m.edges[e].v0 >= m.nverts || m.edges[e].v1 >= m.nverts) {
			printf("  *** FAIL: edge %d out of range\n", e); exit(1);
		}
		if (m.edges[e].v0 == m.edges[e].v1) {
			printf("  *** FAIL: degenerate edge %d\n", e); exit(1);
		}
		if (m.edges[e].v0 > m.edges[e].v1) {
			printf("  *** FAIL: edge %d not normalized\n", e); exit(1);
		}
	}
	for (int a = 0; a < m.nedges; a++)
		for (int b = a + 1; b < m.nedges; b++)
			if (m.edges[a].v0 == m.edges[b].v0 && m.edges[a].v1 == m.edges[b].v1) {
				printf("  *** FAIL: duplicate edge %d/%d\n", a, b); exit(1);
			}
	printf("  edge list: in range, non-degenerate, deduplicated  OK\n");
}

int main(int argc, char **argv) {
	for (int i = 1; i < argc; i++) {
		pumps = 0;
		printf("\n=== %s ===\n", argv[i]);
		if (!stl_load(argv[i], &m, pump)) {
			printf("  LOAD FAILED: %s\n", stl_last_error());
			continue;
		}
		printf("  name=\"%s\" verts=%d/%d edges=%d/%d decimated=%d pumps=%d\n",
			m.name, m.nverts, MODEL_MAX_VERTS,
			m.nedges, MODEL_MAX_EDGES, m.decimated, pumps);
		check_extent();
		render(0.6);
	}
	printf("\nall checks passed\n");
	return 0;
}
