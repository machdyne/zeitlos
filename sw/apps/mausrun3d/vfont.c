/*
 * mausrun3d -- letters of lines (vfont.h).
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * On a chunky picture the runtime draws no text, and T2 draws lines: so
 * the words are lines. A letter is polylines on a grid 4 wide and 6 high
 * (y down), each point two digits, polylines apart by a space.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "zgl.h"
#include "vfont.h"

static const char *glyph(char c) {
	static const char *const az[26] = {
		"060110304146 0343",                          /* A */
		"06003041423344453606 0333",                   /* B */
		"4130100105163645",                           /* C */
		"00304145360600",                             /* D */
		"40000646 0333",                              /* E */
		"40000006 0333",                              /* F */
		"41301001051636454323",                       /* G */
		"0006 4046 0343",                             /* H */
		"1030 2026 1636",                             /* I */
		"4045361605",                                 /* J */
		"0006 400346",                                /* K */
		"000646",                                     /* L */
		"0600234046",                                 /* M */
		"06004640",                                   /* N */
		"103041453616050110",                         /* O */
		"06003041423303",                             /* P */
		"103041453616050110 2446",                    /* Q */
		"06003041423303 2346",                        /* R */
		"413010010213334445361605",                   /* S */
		"0040 2026",                                  /* T */
		"000516364540",                               /* U */
		"002640",                                     /* V */
		"0006234640",                                 /* W */
		"0046 4006",                                  /* X */
		"002340 2326",                                /* Y */
		"00400646",                                   /* Z */
	};
	static const char *const d09[10] = {
		"103041453616050110 4105",                    /* 0 */
		"112026 1636",                                /* 1 */
		"01103041420646",                             /* 2 */
		"0110304142334445361605 1333",                /* 3 */
		"300444 3036",                                /* 4 */
		"4000023243453606",                           /* 5 */
		"30100105163645443303",                       /* 6 */
		"00404116",                                   /* 7 */
		"103041423313020110 1304051636454433",        /* 8 */
		"43130201103041453616",                       /* 9 */
	};
	if (c >= 'a' && c <= 'z') c = (char)(c - 32);
	if (c >= 'A' && c <= 'Z') return az[c - 'A'];
	if (c >= '0' && c <= '9') return d09[c - '0'];
	switch (c) {
	case '!': return "2024 2526";
	case '.': return "2526";
	case ',': return "2517";
	case ':': return "2122 2425";
	case '/': return "4006";
	case '-': return "0343";
	case '?': return "0110304142332324 2526";
	case '\'': return "2021";
	default:  return "";
	}
}

int vf_width(const char *s, int scale) {
	int n = (int)strlen(s);
	return n ? (n * 6 - 2) * scale : 0;
}

void vf_text(int x, int y, int scale, int colour, const char *s, bool bold) {
	glIndexi(colour);
	glBegin(GL_LINES);
	for (; *s; s++, x += 6 * scale) {
		const char *g = glyph(*s);
		int px = -1, py = -1;
		for (; *g; g++) {
			int qx, qy;
			if (*g == ' ') { px = -1; continue; }
			if (!g[1]) break;
			qx = g[0] - '0';
			qy = g[1] - '0';
			g++;
			if (px >= 0) {
				int ax = x + px * scale, ay = y + py * scale, bx = x + qx * scale, by = y + qy * scale;
				glVertex2i(ax, ay); glVertex2i(bx, by);
				if (bold) { glVertex2i(ax + 1, ay); glVertex2i(bx + 1, by); }
			}
			px = qx; py = qy;
		}
	}
	glEnd();
}

void vf_centred(int y, int scale, int colour, const char *s, bool bold) {
	vf_text((320 - vf_width(s, scale)) / 2, y, scale, colour, s, bold);
}
