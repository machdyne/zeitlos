from bdflib import reader

# Load font
with open("6x12.bdf", "rb") as f:
    font = reader.read_bdf(f)

# Map codepoints to glyphs
glyph_dict = {g.codepoint: g for g in font.glyphs if g.codepoint is not None}

# ASCII range
char_start = 32
char_end = 127

# Dimensions
width = 6
height = 12

# Process each glyph
for codepoint in range(char_start, char_end):
    ch = chr(codepoint)
    glyph = glyph_dict.get(codepoint)
    print(f"// Char {codepoint} ({ch})")

    if not glyph or not glyph.data:
        # Print empty box
        for _ in range(height):
            print("0 " * width)
        print()
        continue

    # glyph.data = list of integers (bitmaps), one int per row
    rows = glyph.data

    for y in range(height):
        if y >= len(rows):
            print("0 " * width)
            continue

        row = rows[y]
        bits = f"{row:0{width}b}"[-width:]  # right-align and crop to width
        print(" ".join(bits))

    print()

