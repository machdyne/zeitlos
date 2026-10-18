#ifndef SHEET_CHART_H
#define SHEET_CHART_H

/*
 * Zeitlos -- sheet's charts. See chart.c and docs/sheet_app.md.
 */

#include "../../common/zwin.h"
#include "../../common/zfont.h"
#include "sheet_core.h"

// Draws the sheet's chart into the content-relative rectangle.
void chart_draw(const z_win_t *win, sheet_t *s, const sheet_chart_t *g,
	int x, int y, int w, int h, const z_font_t *font);

#endif
