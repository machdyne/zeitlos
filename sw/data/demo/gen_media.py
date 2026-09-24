#!/usr/bin/env python3
"""
Generates the original demo media in this directory (docs/demo.md):

  zeitlos.svg   a vector clock face for view's SVG renderer
  squirrel.pgm  sw/data/images/squirrel.jpg, grey, 1.6x -- no JPEG
                decoder between the card and the screen (needs Pillow)

    python3 sw/data/demo/gen_media.py

Needs Pillow. Deterministic: rerunning reproduces the same bytes.
"""
import os, math

HERE = os.path.dirname(os.path.abspath(__file__))
W, H = 640, 480

def svg():
    cx, cy, r = 320, 240, 200
    out = ['<?xml version="1.0" encoding="UTF-8"?>',
           '<svg xmlns="http://www.w3.org/2000/svg" width="640" height="480" viewBox="0 0 640 480">',
           '<rect x="0" y="0" width="640" height="480" fill="white"/>',
           f'<circle cx="{cx}" cy="{cy}" r="{r + 16}" fill="black"/>',
           f'<circle cx="{cx}" cy="{cy}" r="{r}" fill="white"/>',
           f'<circle cx="{cx}" cy="{cy}" r="{r - 10}" fill="none" stroke="black" stroke-width="2"/>']
    # minute ticks and hour wedges
    for i in range(60):
        a = math.radians(i * 6)
        big = i % 5 == 0
        r0 = r - (34 if big else 22); r1 = r - 14
        w = 8 if big else 2
        x0, y0 = cx + r0 * math.sin(a), cy - r0 * math.cos(a)
        x1, y1 = cx + r1 * math.sin(a), cy - r1 * math.cos(a)
        out.append(f'<line x1="{x0:.1f}" y1="{y0:.1f}" x2="{x1:.1f}" y2="{y1:.1f}" '
                   f'stroke="black" stroke-width="{w}"/>')
    # a spiral of dots: time running outwards
    for i in range(40):
        a = i * 0.55; rr = 12 + i * 3.2
        out.append(f'<circle cx="{cx + rr * math.cos(a):.1f}" cy="{cy + rr * math.sin(a):.1f}" '
                   f'r="{1.5 + i * 0.12:.1f}" fill="black"/>')
    # hands: star-shaped hour hand with evenodd, curved minute hand
    out.append(f'<g transform="translate({cx},{cy}) rotate(-50)">'
               '<path d="M -9 0 Q 0 -120 9 0 Q 0 22 -9 0 Z" fill="black"/></g>')
    out.append(f'<g transform="translate({cx},{cy}) rotate(75)">'
               '<path d="M -6 0 C -4 -80 4 -80 6 -150 C 10 -80 6 -40 6 0 Q 0 16 -6 0 Z" fill="black"/></g>')
    pts = []
    for i in range(10):
        a = math.radians(i * 36); rr = 26 if i % 2 == 0 else 11
        pts.append(f"{cx + rr * math.sin(a):.1f},{cy - rr * math.cos(a):.1f}")
    out.append(f'<polygon points="{" ".join(pts)}" fill="white" stroke="black" stroke-width="3"/>')
    out.append(f'<circle cx="{cx}" cy="{cy}" r="5" fill="black"/>')
    out.append('</svg>')
    with open(os.path.join(HERE, "zeitlos.svg"), "w") as f:
        f.write("\n".join(out) + "\n")

def squirrel():
    from PIL import Image
    src = os.path.join(HERE, "..", "images", "squirrel.jpg")
    im = Image.open(src).convert("L")
    im = im.resize((im.width * 8 // 5, im.height * 8 // 5), Image.LANCZOS)
    with open(os.path.join(HERE, "squirrel.pgm"), "wb") as f:
        f.write(b"P5\n# Zeitlos demo: sw/data/images/squirrel.jpg (sw/data/demo/gen_media.py)\n")
        f.write(b"%d %d\n255\n" % (im.width, im.height))
        f.write(im.tobytes())

if __name__ == "__main__":
    svg()
    squirrel()
