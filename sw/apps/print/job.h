#ifndef PRINT_JOB_H
#define PRINT_JOB_H

/*
 * print -- a job: the zpaper file an app wrote (sw/common/zpaper.h),
 * its options, and how its pages are laid on sheets. docs/print.md,
 * "Job options" and "Laying pages on sheets".
 */

#include <stdint.h>
#include <stdbool.h>

#include "../../common/zpaper.h"
#include "../../common/zpage.h"

#define JOB_MAX_PAGES	1024
#define JOB_MAX_NUP		16

typedef struct {
	const uint8_t	*data;
	uint32_t		len;
	int				npages;			// in the file
	uint32_t		*start, *bytes;	// each page's records (zp_pages())

	// -- options (zp_job() records; defaults when absent) --
	char		title[64];		// "title"
	char		dest[200];		// "dest": a printer's name, "pdf:/path", "ps:/path";
								// "" the default printer
	char		paper[16];		// "paper": zpage name; "" the first page's own
								// paper, else default_paper
	char		default_paper[16];	// not an option: the service's setting
								// (apps.print.paper), "letter" if unset
	int			copies;			// "copies", 1..99
	int			nup;			// "nup": 1, 2, 4, 6, 8, 9, 12, 16
	int			mode;			// "size": "fit" ZPAGE_FIT, "actual" ZPAGE_ACTUAL
	bool		cut;			// "cut": cut marks between cells
	bool		frame;			// "frame": a hairline round each page
	int			gap_mm10;		// "gap": gutter between cells, tenths of mm
	int			margin_mm10;	// "margin": from each edge of the sheet
	int			from, to;		// "pages": "3", "2-5"; 1-based, 0 = all
	char		quality[8];		// "quality": "draft", "normal", "high"
	char		media_type[24];	// "media-type": "stationery", "photographic" ...
	char		media_source[24];// "media-source": "auto", "main", "rear" ...
	char		trays[96];		// not an option: the service's setting
								// (apps.print.trays), "a4 main; a5 rear labels"
} job_t;

// Reads the options and finds the pages. `start` and `bytes` hold
// JOB_MAX_PAGES each. False if it is not a job (or has no pages).
bool job_open(job_t *j, const uint8_t *data, uint32_t len, uint32_t *start,
	uint32_t *bytes);

// The pages that print, in order: first and count (after "pages").
void job_range(const job_t *j, int *first, int *count);
int job_sheets(const job_t *j);

// The paper a sheet is: the option, else the first page's paper, else
// `fallback`, else j->default_paper.
const zpage_size_t *job_paper(const job_t *j, const char *fallback);

// The page's shape, read from its PAGE record.
bool job_page(const job_t *j, int index, zpage_t *pg);

// Sheet `s` (0-based): which pages go on it, and where. Returns how
// many (0 if past the end). `sh` is the sheet at the device resolution
// in use.
int job_sheet(const job_t *j, int s, const zpage_sheet_t *sh, int *page,
	zpage_cell_t *cell);

// The sheet for a paper at `dpi`, portrait as it feeds, with the job's
// margin -- or the printer's (left, top, right, bottom, tenths of a
// mm), whichever is larger. Pages are turned onto it, not the sheet.
void job_sheet_geometry(const job_t *j, const zpage_size_t *paper, int dpi,
	const int printer_mm10[4], zpage_sheet_t *sh);

// Sheet s at `dpi`, all in one: the paper (job_paper(), or with paper
// "page" a sheet the size of the sheet's first page -- a PDF of a
// slide deck -- with no margin) and its geometry. *paper is the paper,
// or NULL for a page-sized sheet; *w_mm10, *h_mm10 its size as it
// lies (landscape for a landscape page-sized sheet).
void job_sheet_at(const job_t *j, int s, int dpi, const char *fallback,
	const int printer_mm10[4], zpage_sheet_t *sh, const zpage_size_t **paper,
	int *w_mm10, int *h_mm10);

// PWG media name for a zpage paper ("na_letter_8.5x11in"); a custom
// size gets a "custom_zeitlos_WxHmm" name.
void job_media_name(const zpage_size_t *paper, char *out, int n);

#endif
