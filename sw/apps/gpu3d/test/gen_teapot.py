#!/usr/bin/env python3
"""Generate a teapot-shaped STL (ASCII + binary) for parser testing.

Body/lid are surfaces of revolution over a hand-drawn profile; the
spout is a tapered swept tube; the handle is a torus arc. Not the real
Newell teapot, but the same topology class (closed-ish shell, shared
vertices, curved surfaces) and tuned to land near 400KB in ASCII.
"""
import math, struct, sys

tris = []

def add_quad(a, b, c, d):
    tris.append((a, b, c))
    tris.append((a, c, d))

def revolve(profile, seg, closed_bottom=False):
    """profile: list of (r, y) from bottom to top."""
    ring = []
    for (r, y) in profile:
        ring.append([(r * math.cos(2 * math.pi * i / seg),
                      y,
                      r * math.sin(2 * math.pi * i / seg)) for i in range(seg)])
    for k in range(len(ring) - 1):
        lo, hi = ring[k], ring[k + 1]
        for i in range(seg):
            j = (i + 1) % seg
            add_quad(lo[i], lo[j], hi[j], hi[i])
    if closed_bottom:
        cx = (0.0, profile[0][1], 0.0)
        lo = ring[0]
        for i in range(seg):
            j = (i + 1) % seg
            tris.append((cx, lo[j], lo[i]))
    return ring

D = float(sys.argv[1]) if len(sys.argv)>1 else 1.0
SEG = max(6, int(48*D))

# --- body: bulbous pot ---
body = []
NB = max(5, int(25*D))
for t in range(NB):
    u = t / (NB-1.0)
    y = -1.0 + 2.0 * u
    r = 1.35 * math.sqrt(max(0.0, 1.0 - (y * 0.92) ** 2)) * (1.0 - 0.18 * u)
    r = max(r, 0.28)
    body.append((r, y))
revolve(body, SEG, closed_bottom=True)

# --- lid ---
lid = []
NL = max(4, int(12*D))
for t in range(NL):
    u = t / (NL-1.0)
    y = 1.0 + 0.42 * u
    r = 0.92 * math.cos(u * math.pi / 2.1) + 0.06
    lid.append((max(r, 0.05), y))
lid.append((0.10, 1.50))
lid.append((0.20, 1.60))
lid.append((0.05, 1.70))
revolve(lid, SEG)

# --- spout: tapered tube along a curve ---
def tube(path, radii, seg):
    rings = []
    for idx, (p, rad) in enumerate(zip(path, radii)):
        if idx + 1 < len(path):
            d = [path[idx + 1][k] - p[k] for k in range(3)]
        else:
            d = [p[k] - path[idx - 1][k] for k in range(3)]
        n = math.sqrt(sum(v * v for v in d)) or 1.0
        d = [v / n for v in d]
        up = [0.0, 0.0, 1.0]
        ax = [d[1] * up[2] - d[2] * up[1],
              d[2] * up[0] - d[0] * up[2],
              d[0] * up[1] - d[1] * up[0]]
        na = math.sqrt(sum(v * v for v in ax)) or 1.0
        ax = [v / na for v in ax]
        ay = [d[1] * ax[2] - d[2] * ax[1],
              d[2] * ax[0] - d[0] * ax[2],
              d[0] * ax[1] - d[1] * ax[0]]
        ring = []
        for i in range(seg):
            a = 2 * math.pi * i / seg
            ring.append(tuple(p[k] + rad * (math.cos(a) * ax[k] + math.sin(a) * ay[k])
                              for k in range(3)))
        rings.append(ring)
    for k in range(len(rings) - 1):
        lo, hi = rings[k], rings[k + 1]
        for i in range(seg):
            j = (i + 1) % seg
            add_quad(lo[i], lo[j], hi[j], hi[i])

spath, srad = [], []
NS = max(5, int(16*D))
for t in range(NS):
    u = t / (NS-1.0)
    spath.append((1.05 + 1.15 * u, -0.30 + 1.25 * u * u, 0.0))
    srad.append(0.30 * (1.0 - 0.62 * u))
tube(spath, srad, max(6,int(24*D)))

# --- handle: torus arc ---
hpath, hrad = [], []
NH = max(6, int(20*D))
for t in range(NH):
    u = t / (NH-1.0)
    a = math.pi * (0.30 + 1.40 * u)
    hpath.append((-1.05 - 0.72 * math.sin(a), 0.55 - 0.80 * math.cos(a), 0.0))
    hrad.append(0.15)
tube(hpath, hrad, max(6,int(20*D)))

sys.stderr.write("triangles: %d\n" % len(tris))


def write_ascii(path):
    with open(path, "w") as f:
        f.write("solid teapot\n")
        for (a, b, c) in tris:
            ux, uy, uz = [b[i] - a[i] for i in range(3)]
            vx, vy, vz = [c[i] - a[i] for i in range(3)]
            nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
            n = math.sqrt(nx * nx + ny * ny + nz * nz) or 1.0
            f.write("  facet normal %.6e %.6e %.6e\n" % (nx / n, ny / n, nz / n))
            f.write("    outer loop\n")
            for v in (a, b, c):
                f.write("      vertex %.6e %.6e %.6e\n" % v)
            f.write("    endloop\n")
            f.write("  endfacet\n")
        f.write("endsolid teapot\n")


def write_binary(path):
    with open(path, "wb") as f:
        f.write(b"binary teapot" + b"\0" * (80 - 13))
        f.write(struct.pack("<I", len(tris)))
        for (a, b, c) in tris:
            f.write(struct.pack("<3f", 0.0, 0.0, 0.0))
            for v in (a, b, c):
                f.write(struct.pack("<3f", *v))
            f.write(struct.pack("<H", 0))


write_ascii(sys.argv[2])
write_binary(sys.argv[3])
