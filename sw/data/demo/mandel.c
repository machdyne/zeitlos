/* Mandelbrot, 16 colours, full screen. Built on the board by zcc. */
/* No #include: the three runtime calls and the registers, by hand. */
int printf(const char *fmt, ...);
int z_pid_lookup(const char *name, unsigned *pid);
int z_msg_send_u32(unsigned to, unsigned subject, unsigned tag, unsigned v);

#define REG(a) (*(volatile unsigned *)(a))
#define GAME   REG(0x7000020c)          /* game mode on/off */
#define VIEW   REG(0x70000210)          /* viewport x, y */
#define FRAME  REG(0x70000214)          /* frame counter */
#define COLOR  REG(0x70000224)          /* colour mode */
#define PAL    REG(0x70000234)          /* palette: index << 24 | rgb */
#define FB     ((volatile unsigned *)0x20000000)
#define GRAB   -32760                   /* wm: take / give back the screen */
#define FREE   -32759

int hue[15] = { 0xf00, 0xf60, 0xfc0, 0xcf0, 0x6f0, 0x0f3, 0x0f9, 0x0ff,
  0x09f, 0x03f, 0x30f, 0x90f, 0xf0f, 0xf09, 0xf03 };

int dot(int cr, int ci) {             /* 4.12 fixed point */
  int zr = 0, zi = 0, n, r, i, x = cr - 1024;
  r = (x * x >> 12) + (ci * ci >> 12);   /* inside the main cardioid? */
  if ((r * (r + x) >> 12) < (ci * ci >> 14)) return 0;
  x = cr + 4096;                          /* or the bulb to its left? */
  if ((x * x >> 12) + (ci * ci >> 12) < 256) return 0;
  for (n = 0; n < 32; n++) {
    r = zr * zr >> 12; i = zi * zi >> 12;
    if (r + i > 16384) return n % 15 + 1;
    zi = (zr * zi >> 11) + ci; zr = r - i + cr;
  }
  return 0;                           /* inside: black */
}

int main(void) {
  unsigned wm, f, m, p0, p1, p2, p3; int x, y, b, c, k, t, u;
  if ((COLOR >> 16) != 0x5a50 || !(COLOR & 0x8000)) {
    printf("mandel: no colour here\n"); return 1;
  }
  z_pid_lookup("wm0", &wm); z_msg_send_u32(wm, GRAB, 0, 0);
  for (k = 0; k < 4; k++) {           /* let wm stop drawing: 4 frames */
    f = FRAME & 0xffff; while ((FRAME & 0xffff) == f) ;
  }
  for (k = 0; k < 9600; k++) FB[k] = 0;
  for (k = 0; k < 16; k++) PAL = k << 24 | (k ? hue[k - 1] : 0);
  VIEW = 0; GAME = 1;
  COLOR = 1 | 3 << 1 | 0x39 << 8;     /* on, 4 planes, one per quadrant */
  for (y = 0; y < 120; y++) for (x = 0; x < 320; x += 32) {
    p0 = p1 = p2 = p3 = 0;
    for (b = 0; b < 32; b++) {
      c = dot((x + b) * 40 - 9267, y * 40 - 4780);
      if (c) {
        m = 1u << b;
        if (c & 1) p0 |= m; if (c & 2) p1 |= m;
        if (c & 4) p2 |= m; if (c & 8) p3 |= m;
      }
    }
    t = y * 20 + (x >> 5); u = (239 - y) * 20 + (x >> 5);
    FB[t] = p0;        FB[t + 10] = p1;        /* plane k: a quadrant */
    FB[t + 4800] = p2; FB[t + 4810] = p3;
    FB[u] = p0;        FB[u + 10] = p1;        /* and its mirror image */
    FB[u + 4800] = p2; FB[u + 4810] = p3;
  }
  for (f = 0; f < 240; f++) {         /* colour cycling, 4 s */
    k = FRAME & 0xffff; while ((FRAME & 0xffff) == k) ;
    for (k = 1; k < 16; k++) PAL = k << 24 | hue[(k - 1 + f / 4) % 15];
  }
  COLOR = 0; GAME = 0;
  z_msg_send_u32(wm, FREE, 0, 0);
  return 0;
}
