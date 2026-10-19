#ifndef PRINT_IPP_H
#define PRINT_IPP_H

/*
 * print -- IPP (RFC 8010/8011) over HTTP/1.1: what a driverless printer
 * speaks on port 631. Two operations: Get-Printer-Attributes, to learn
 * what the printer takes (formats, resolutions, paper, margins), and
 * Print-Job, the document streamed after the request's attributes in
 * the same HTTP body, chunked because a raster's length is not known
 * until it is made. docs/print.md, "IPP".
 *
 * The transport is the caller's: a write and a read on a connected
 * socket (net's raw socket on the board, POSIX in the host tests).
 */

#include <stdint.h>
#include <stdbool.h>

#include "out.h"

// -- addresses -----------------------------------------------------------

enum { URL_IPP = 0, URL_SOCKET = 1, URL_HTTP = 2 };

typedef struct {
	int		kind;			// URL_*
	char	host[64];
	int		port;			// 631 for ipp, 9100 for socket
	char	path[96];		// "/ipp/print"
} ipp_url_t;

// "ipp://host[:port]/path", "http://...", "socket://host[:port]",
// "host" alone (ipp, /ipp/print). False if it is none of these.
bool ipp_url(const char *s, ipp_url_t *u);

// -- HTTP -------------------------------------------------------------------

// Bytes from the printer: up to max, waiting as long as the transport
// thinks reasonable; 0 when the connection closed, -1 on failure.
typedef int (*net_read_fn)(void *ctx, uint8_t *buf, int max);

typedef struct {
	out_t			*sock;		// writes to the connection
	net_read_fn		read;
	void			*rctx;
	out_t			body;		// the request body: chunked onto sock
	uint8_t			buf[2048];
	int				n;
	// reading the response
	uint8_t			rbuf[1024];
	int				rlen, rpos;
} http_t;

bool http_post_begin(http_t *h, const ipp_url_t *u, const char *content_type);
out_t *http_body(http_t *h);
bool http_post_end(http_t *h);

// Reads the response: its status (200 ...) and body (up to max bytes,
// chunked or not). Returns the body's length, or -1.
int http_response(http_t *h, int *status, uint8_t *body, int max);

// -- IPP --------------------------------------------------------------------------

typedef struct {
	int		status;					// IPP status-code (0x0000.. ok)
	char	message[96];			// status-message
	char	make[64];				// printer-make-and-model
	char	state_reasons[96];		// printer-state-reasons, joined
	int		state;					// 3 idle, 4 processing, 5 stopped
	bool	pwg, pdf, ps, urf, jpeg;	// document formats it takes
	bool	sgray8, srgb8;			// pwg-raster-document-type-supported
	int		dpi[4], ndpi;			// pwg-raster-document-resolution-supported
	char	media_default[48];
	char	media_ready[4][48];
	int		nready;
	int		margin[4];				// left, top, right, bottom, 1/100 mm: the
									// largest each lists (not borderless)
	bool	has_margins;
	int		job_id;
	int		job_state;
} ipp_info_t;

// A Get-Printer-Attributes request (no document): returns its length.
uint32_t ipp_get_attributes(uint8_t *buf, uint32_t max, const char *uri);

typedef struct {
	const char	*uri;
	const char	*user;			// requesting-user-name
	const char	*job_name;
	const char	*format;		// "image/pwg-raster", "application/pdf" ...
	const char	*media;			// PWG name, or NULL
	int			media_w, media_h;	// its size, 1/100 mm (for media-col)
	const char	*media_type;	// or NULL
	const char	*media_source;	// or NULL
	int			quality;		// 3, 4, 5, or 0
	int			copies;			// sent only when > 1
	bool		mono;			// print-color-mode monochrome
} ipp_job_t;

// The Print-Job request's attributes; the document follows on `o`.
bool ipp_print_job(out_t *o, const ipp_job_t *j);

// Reads a response body. False if it is not IPP.
bool ipp_parse(const uint8_t *d, int n, ipp_info_t *info);

// "successful-ok", "client-error-document-format-not-supported" ...
const char *ipp_status_name(int status);

#endif
