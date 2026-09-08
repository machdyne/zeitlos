/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * web -- an experimental text-mode web browser, and the system's
 * fetch service. See docs/web_app.md and docs/http.md.
 *
 * -- what is in this file, and what deliberately is not --
 *
 * This file is the parts that need the machine: a window, a message
 * loop, a socket, a file on the card. Everything with interesting
 * logic in it lives next door and is tested on the build machine:
 *
 *   url.c      RFC 3986 parsing and resolution      tests/test_url.c
 *   http.c     request building, response parsing   tests/test_http.c
 *   uni.c      UTF-8 folded to ASCII                tests/test_html.c
 *   html.c     bytes to blocks                      tests/test_html.c
 *   layout.c   blocks to pixels                     tests/test_layout.c
 *   page.c     the index and scroll arithmetic      tests/test_page.c
 *
 * That split is the whole strategy. A browser has more places to be
 * subtly wrong than anything else in this tree, and almost none of
 * them need hardware to be wrong on.
 *
 * -- how a page is fetched --
 *
 *   resolve the host        z_dns_resolve()   (blocks, bounded)
 *   open a raw socket       Z_PORT_CONNECT {ip, port} to net
 *   send the request        http_build_request()
 *   feed the response       http_feed()
 *   spool the body          fs_write_chunk() to /web/cache
 *   index it                page_index_more(), a few blocks at a time
 *
 * The body is spooled to the card and only then indexed, rather than
 * being parsed as it arrives. That means no progressive rendering in
 * this drop: a slow page shows a byte count and then appears all at
 * once. It buys not needing a read handle and a write handle open on
 * the same file simultaneously, which is a question about the
 * filesystem this app should not be the first thing to ask.
 *
 * page.c is written for the streaming case regardless and its tests
 * cover it, so turning progressive rendering on later is a change
 * here, not there.
 *
 * -- https --
 *
 * Fully verified: the chain is checked against a root store on the
 * card, the hostname must match, the dates must hold, and the server
 * must prove it holds the leaf's private key. Any of those failing
 * refuses the connection and says which one. See docs/tls.md.
 *
 * The WEB_TLS_INSECURE flag that existed while the verification
 * machinery was being written is GONE. There is no build of this app
 * that speaks TLS without checking certificates.
 *
 * Two things must be present, and their absence is a refusal rather
 * than a downgrade:
 *
 *   /web/roots.der   concatenated DER certificates, the trust store
 *   a set clock      z_rtc_valid(), which `net` gets from NTP
 *
 * Without a clock an expired certificate cannot be told from a
 * current one, and an expired certificate is the normal state of one
 * whose key has since been compromised.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "../../common/zeitlos.h"
#include "../../common/zwin.h"
#include "../../common/zwidget.h"
#include "../../common/zwm.h"
#include "../../common/zkbd.h"
#include "../../common/zgfx.h"
#include "../../common/zfont.h"
#include "../../common/zport.h"
#include "../../common/zstream.h"
#include "../../common/znet.h"
#include "../../common/zweb.h"
#include "../../common/zdns.h"
#include "../../common/zfsapp.h"

#include "url.h"
#include "../../common/zedit.h"
#include "../../common/zinflate.h"
#include "../../common/zimg.h"
#if WEB_SVG
#include "../../common/zsvg.h"
#endif
#include "toolbar.h"
#include "http.h"
#if WEB_TLS
#include "tls.h"
#include "x509.h"
#include "../../common/zrtc.h"
#endif
#include "html.h"
#include "layout.h"
#include "page.h"

// The default window, on a 640x480 screen.
//
// Deliberately not full screen: this is a multitasking system with a
// dock, and a browser that opens covering everything behaves like it
// is the only thing running. At 5x8 the content area is still around
// 100 columns, which is wider than the 80 that most prose was written
// for -- the constraint on readability here is the font, not the
// width.
#define WIN_W  540
#define WIN_H  400

// Height of the location strip at the top of the content area: one
// line of body text plus a pixel above and below plus the rule.
#define BAR_H  TB_H

// Cache directory and the single spool file.
//
// One file, not one per URL. A URL cache needs eviction, a name
// mapping and a size budget; a single spool gives back/forward
// nothing and gives everything else -- re-wrapping on resize, serving
// the fetch service from the same bytes the renderer uses -- for a
// fixed 0 bytes of bookkeeping. A real cache is a Phase 4 item.
#define CACHE_DIR   "/web"
// The spool lives on the RAM disk when there is one.
//
// Measured on hardware: indexing a 258KB page meant re-reading it off
// the card at about 19 KB/s -- 13 seconds, against 1.4 for writing
// it. The spool is scratch for one page and has no business being on
// durable storage.
//
// PREFERRED, not required. /ram is absent on a 1MB board (kernel.c
// does not create one there) and can be turned off, and `web` still
// works from the card when it is -- slowly, which the timing lines
// will say plainly. Choosing at runtime also makes the two directly
// comparable: same build, same page, one variable.
#define SPOOL_PATH_RAM   "/ram/webspool"
#define SPOOL_PATH_CARD  "/web/spool"

static const char *spool_path = SPOOL_PATH_CARD;

// Redirect budget. Eight is what every mainstream browser uses; a
// site that needs more is looping.
#define MAX_REDIRECTS 8

// Blocks indexed per idle pass.
//
// Small on purpose. Indexing a 350KB page takes hundreds of
// milliseconds, and doing it in one go means the window does not
// repaint and wm starts reporting a missed redraw ack -- the same
// failure term's connect path hit (zport.h).
#define INDEX_BUDGET  24

// Eight, not thirty-two.
//
// Each entry is a URL_MAX buffer, so thirty-two was 32KB of .bss for
// a depth nobody reaches with two buttons and no menu. Eight covers
// the way this is actually used -- follow a few links, go back --
// and the oldest is dropped rather than refusing to record a new
// one.
#define HIST_MAX 8

static z_win_t win;
static z_scrollbar_t sbar;

static char self_name[24];
static uint32_t net_pid;

// -- the document --------------------------------------------------

static page_t pg;
static layout_cfg_t cfg;
static page_pos_t top;

static url_t cur_url;			// what is on screen (or being fetched)
static char cur_url_text[URL_MAX];

static int spool_rd = -1;		// read handle, open while a page is live
static int spool_wr = -1;

// This fetch is an image for an in-place placeholder, not a page.
//
// Declared here rather than beside the image state below because the
// spool functions above the image code need it: an image fetch writes
// to its own handle and must not touch the page's.
static bool fetching_image;

// The image spool's write handle, separate from the page's.
static int img_wr = -1;
static uint32_t spool_len;

// Milliseconds since boot, for verify.c's timing report.
static uint32_t web_ms(void) {
	return (uint32_t)((uint64_t)z_uptime_ticks() * 1000u / 732u);
}

// -- phase timing --
//
// Every stage of a page load, printed when the page is ready.
//
// The point is to make optimisation arguable rather than intuitive.
// The handshake turned out to be 63 seconds and the root store 1.7,
// and both were guesses until they were measured. The body and index
// figures are the ones with nothing behind them yet.
static struct {
	uint32_t	t_fetch;		// start of this navigation
	uint32_t	t_dns;			// resolving the name
	uint32_t	t_connect;		// opening the socket
	uint32_t	t_ready;		// the TLS handshake
	uint32_t	t_body;			// receiving the body
	uint32_t	spool_ms;		// inside card writes
	uint32_t	crypto_ms;		// inside tls_feed (decrypt and parse)
	uint32_t	index_ms;		// inside page_index_more
	uint32_t	read_ms;		// of that, inside the spool read
	uint32_t	body_bytes;
} tm;

// fs_read_chunk() is sequential from wherever the handle sits, and
// fs_seek() is a syscall. page.c reads forward within a replay, so
// tracking the handle's position and seeking only when it actually
// differs turns most reads into a single syscall instead of two.
// Same optimisation sw/apps/read's rd_seek() makes, for the same
// reason: card I/O is where a reader on this machine spends its time.
static uint32_t spool_pos;

// Writes are batched through this rather than going to the card one
// HTTP chunk at a time.
//
// on_body() is called with whatever a record decrypted to -- often a
// kilobyte -- and an SD write has a fixed cost per call
// that dwarfs the bytes. net's queue filling at 34KB while the app
// had relayed only 25KB is that cost: the card, not the network, was
// setting the pace.
// 16KB, matching a maximum TLS record.
//
// An SD write costs far more per CALL than per byte, so the
// batch size is close to a direct multiplier on how fast the body can
// be drained -- and the body is drained on the same thread that has
// to keep acking net.
#define SPOOL_BUF 16384
static uint8_t spool_wbuf[SPOOL_BUF];
static uint32_t spool_wlen;

static bool spool_flush(void) {

	uint32_t t0;
	int rv;

	if (spool_wlen == 0) return true;
	if ((fetching_image ? img_wr : spool_wr) < 0) return false;

	t0 = web_ms();
	rv = fs_write_chunk(fetching_image ? img_wr : spool_wr,
		(const char *)spool_wbuf, (int)spool_wlen);
	tm.spool_ms += web_ms() - t0;

	if (rv < 0) return false;

	spool_wlen = 0;
	return true;

}

static uint32_t spool_read(void *user, uint32_t off, char *buf, uint32_t len) {

	int n;

	(void)user;

	if (spool_rd < 0) return 0;
	if (off >= spool_len) return 0;
	if (off + len > spool_len) len = spool_len - off;

	{
		uint32_t t0 = web_ms();

		if (off != spool_pos) {
			if (fs_seek(spool_rd, off) < 0) return 0;
			spool_pos = off;
		}

		n = fs_read_chunk(spool_rd, buf, (int)len);
		tm.read_ms += web_ms() - t0;
	}
	if (n <= 0) return 0;

	spool_pos += (uint32_t)n;
	return (uint32_t)n;

}

// -- history -------------------------------------------------------
//
// URLs as text, not url_t. A url_t is about 1.4KB and 32 of them is
// 45KB of .bss for something that is re-parsed in microseconds.
// -- the toolbar --
//
// An editable URL field with Back and Forward beside it, rather than
// a label and a keypress that opens a dialog.
//
// The field is sw/common/zedit.c, the same widget the file dialog
// uses. It was private to zdialog.c until this became its second
// caller, which is exactly the condition that file's own comment set
// for moving it.
static char url_buf[URL_MAX];
static z_edit_t url_edit;

// Typing goes to the field; otherwise keys scroll the page. Clicking
// the field takes focus, clicking the page gives it back.
static bool url_focus;

// The URL bar shows progress while a page loads, and the text being
// typed the rest of the time. Without this, a status message would
// overwrite something half-typed.
static bool url_editing;

// Forgets any loaded image. Called whenever the page changes, since
// a block number means nothing once a different document is loaded.
// -- in-place images --
//
// ONE decoded image is held at a time, at the size of its placeholder
// box. Clicking a different image replaces it.
//
// One, because a decoded bitmap is the size of the box it fills and
// there is no dynamic memory: a page of twenty thumbnails would need
// a budget nobody can state in advance. One is the size that is
// always affordable and is what a reader actually looks at.
//
// The bitmap is sized for the largest box layout.c will produce -- a
// full content width by the 24-line height cap -- so any box fits
// whatever the window is.
#define IMG_MAX_W    640
#define IMG_MAX_H    (24 * 12)
#define IMG_WPL      ((IMG_MAX_W + 31) / 32)

static uint32_t img_bits[IMG_WPL * IMG_MAX_H];

static uint32_t img_block;			// which block it belongs to
static char     img_src[URL_MAX];	// its src, as the markup gave it
static int      img_w, img_h;		// as drawn
static bool     img_have;
static bool     img_loading;
static char     img_status[64];

// The image spool. A SEPARATE file from the page's, because the page
// is still being read from its own -- page.c fetches blocks on demand
// rather than holding the document in memory, so reusing the spool
// would destroy the page the image belongs to.
static char img_spool_path[URL_MAX];
static int  img_fd = -1;			// read, during decode

// The box this block's placeholder occupies, in pixels.
//
// Asked of layout.c rather than recomputed here: the decode has to
// produce exactly what will be drawn, and two pieces of code deriving
// the same size independently is how a picture ends up one pixel
// wider than the frame around it.
static void img_box_size(uint32_t block, int *w, int *h) {

	// The block is no longer consulted: the decode size does not
	// depend on what the markup declared. Kept as a parameter so the
	// call site reads as "the box for this block".
	(void)block;

	// The size to DECODE INTO, which is NOT the placeholder's size.
	//
	// The decoder is given the whole area the image could occupy,
	// never the declared one, and the drawn box then follows whatever
	// came back (see cfg.img_src in layout.h).
	//
	// Honouring the declared size looked more respectful of the page
	// and was wrong, because zimg scales by POWERS OF TWO only. Real
	// markup does not match its files: en.wikipedia.org serves a
	// 120x179 thumbnail in a tag that says width="115" height="171".
	// Five pixels too wide to fit, no 115/120 scale available, so the
	// decoder dropped to 1/2 and produced 60x89 -- a quarter of the
	// area, for a five-pixel discrepancy nobody could see.
	//
	// The declared size still sizes the PLACEHOLDER, which is what it
	// is good for: it says how much room to leave before anything has
	// been loaded.
	*w = cfg.width > 0 ? cfg.width : 160;
	if (*w > IMG_MAX_W) *w = IMG_MAX_W;
	*h = IMG_MAX_H;

}

// zimg's byte source. Reads from the spooled image file.
static int img_read(void *ctx, uint8_t *buf, int len) {
	(void)ctx;
	if (img_fd < 0) return 0;
	return fs_read_chunk(img_fd, (char *)buf, len);
}

// Decodes what was spooled into img_bits, scaled to the box.
//
// zimg downscales by powers of two only, so an image is drawn at the
// largest 1/2^n that fits the placeholder rather than at exactly the
// declared size. A box that says 320x120 may therefore hold a 300x113
// picture -- which is honest about what was decoded, and better than
// resampling a dithered bitmap.
#if WEB_SVG

// Renders a spooled SVG into img_bits.
//
// Fitted to the placeholder box, which for a vector drawing is not a
// compromise: there is no native pixel size to lose. A logo declared
// 24x24 in the markup is drawn at 24x24 and looks right, where the
// same logo as a PNG would have been a blurry thumbnail.
static bool img_render_svg(int box_w, int box_h) {

	static char text[WEB_SVG_MAX];
	static z_svg_edge_t edges[WEB_SVG_EDGES];

	z_svg_t sv;
	int n, rv;

	if (fs_seek(img_fd, 0) < 0) return false;

	n = fs_read_chunk(img_fd, text, WEB_SVG_MAX);
	if (n <= 0) {
		snprintf(img_status, sizeof(img_status), "empty file");
		return false;
	}

	if (n >= WEB_SVG_MAX) {
		snprintf(img_status, sizeof(img_status), "SVG too large");
		return false;
	}

	if (box_w > IMG_MAX_W) box_w = IMG_MAX_W;
	if (box_h > IMG_MAX_H) box_h = IMG_MAX_H;

	memset(&sv, 0, sizeof(sv));
	sv.doc = text;
	sv.len = (uint32_t)n;
	sv.x = 0; sv.y = 0;
	sv.w = box_w; sv.h = box_h;
	sv.edges = edges;
	sv.max_edges = WEB_SVG_EDGES;

	memset(img_bits, 0, sizeof(img_bits));

	rv = z_svg_render_bitmap(&sv, img_bits, IMG_WPL);

	if (rv != Z_SVG_OK) {
		snprintf(img_status, sizeof(img_status), "%s",
			z_svg_strerror(rv));
		return false;
	}

	img_w = box_w;
	img_h = box_h;

	printf("web: image SVG %.0fx%.0f -> %dx%d, %d shapes, %d edges\n",
		sv.src_w, sv.src_h, img_w, img_h, sv.n_shapes, sv.n_edges);

	return true;

}

#endif

static bool img_decode_spooled(int box_w, int box_h) {

	z_img_t im;
	// 1KB. SVG is text, and its <svg> tag sits after an XML
	// declaration, a DOCTYPE and often a generator comment -- offset
	// 114 in an ordinary Inkscape file. A 16-byte sniff, which is all
	// a raster magic number needs, reported every real SVG as
	// unrecognised.
	static uint8_t hdr[1024];
	z_img_fmt_t fmt;
	int n, rv;

	if (img_fd < 0) return false;
	if (fs_seek(img_fd, 0) < 0) return false;

	n = fs_read_chunk(img_fd, (char *)hdr, (int)sizeof(hdr));
	if (n <= 0) { snprintf(img_status, sizeof(img_status), "empty file"); return false; }

#if WEB_SVG
	// SVG first: it is text, not one of zimg's formats, and it is
	// what site logos and Wikipedia's diagrams actually are.
	if (z_svg_sniff((const char *)hdr, (uint32_t)n))
		return img_render_svg(box_w, box_h);
#endif

	fmt = z_img_sniff(hdr, n);
	if (fmt == Z_IMG_FMT_NONE) {
		snprintf(img_status, sizeof(img_status), "unrecognised image format");
		return false;
	}

	if (fs_seek(img_fd, 0) < 0) return false;

	if (box_w > IMG_MAX_W) box_w = IMG_MAX_W;
	if (box_h > IMG_MAX_H) box_h = IMG_MAX_H;

	memset(&im, 0, sizeof(im));
	im.read = img_read;
	im.ctx = NULL;
	im.doc = img_bits;
	im.doc_wpl = IMG_WPL;
	im.doc_w = box_w;
	im.doc_h = box_h;

	z_img_clear(&im);

	rv = z_img_decode(&im, fmt);
	if (rv != Z_IMG_OK) {
		snprintf(img_status, sizeof(img_status), "%s", z_img_strerror(rv));
		return false;
	}

	img_w = im.out_w;
	img_h = im.out_h;
	if (img_w > box_w) img_w = box_w;
	if (img_h > box_h) img_h = box_h;

	// The BOX is printed too. Without it a scaled-down image is a
	// mystery: the only thing that decides the scale is whether the
	// source fits the box (zimg.c's fit_scale()), so the box is the
	// number that explains the result.
	printf("web: image %s %dx%d -> %dx%d (1/%d) box %dx%d%s\n",
		z_img_fmt_name(fmt), im.src_w, im.src_h, img_w, img_h,
		1 << im.shift, box_w, box_h,
		im.was_ordered ? ", ordered dither" : "");

	return true;

}

static void img_forget(void) {
	img_have = false;
	img_loading = false;
	img_src[0] = '\0';
	cfg.img_src = NULL;
	img_status[0] = '\0';
}

static char hist[HIST_MAX][URL_MAX];
static int hist_n;
static int hist_at = -1;

// -- fetch state ---------------------------------------------------

typedef enum {
	W_IDLE = 0,
	W_RESOLVING,
	W_CONNECTING,
	W_RECEIVING,
	W_IMG,				// fetching an image for an in-place placeholder
	W_INDEXING,
	W_READY,
	W_ERROR,
} web_state_t;

static web_state_t state;

// Sized to hold a whole URL and a sentence about it.
//
// It was 128, which is fine for "resolving example.com..." and not
// fine for "the site redirected to <url> -- https needs TLS...". The
// compiler says so (-Wformat-truncation), and the truncation is
// silent and safe, which is exactly why it would have shipped: the
// message would simply have stopped before naming the URL that is
// the only useful part of it.
static char status[URL_MAX + 128];

static z_port_t sock;
static bool sock_open;

// -- connection reuse --
//
// The origin the open socket belongs to, and whether the last
// response left it usable. A fetch to the same origin skips DNS, the
// TCP connect and -- the expensive part -- the whole TLS handshake.
//
// That is worth about fifteen seconds on this hardware. A same-host
// redirect used to pay for a second full handshake to a server it was
// already connected to, because http.c sent `Connection: close` on
// the reasoning that net has one TCB. Keeping the connection open
// uses that SAME one TCB; it simply does not close it.
//
// Kept as strings rather than a url_t: only the origin matters, and
// comparing three fields is clearer than comparing a struct with
// paths and queries in it.
static char conn_host[URL_HOST_MAX];
static uint16_t conn_port;
static bool conn_secure;
static bool conn_reusable;

// This fetch went out on an already-open socket.
//
// A server may close an idle connection at any moment, and it is
// entirely allowed to do so between our last read and our next
// write -- so a reused connection that dies without answering is
// NORMAL, not an error. It is retried once on a fresh socket.
//
// Only once, and only when nothing was received: a connection that
// died mid-response is a real failure, and retrying a request that
// may have been acted on is not something to do quietly.
static bool fetch_reused;
static bool fetch_retrying;

// When the idle connection should be dropped, in z_uptime_ticks().
//
// net has ONE TCB. Holding it open indefinitely for a page nobody is
// going to click on would deny it to every other app, which is what
// the original `Connection: close` was really protecting against.
// Ten seconds keeps it for the redirects and sub-fetches that follow
// a page load, and gives it back after that.
static uint32_t conn_idle_deadline;

#define CONN_IDLE_TICKS  (10u * 732u)

static void conn_forget(void) {
	conn_host[0] = '\0';
	conn_reusable = false;
	conn_idle_deadline = 0;
}

// Can the open socket carry this fetch?
static bool conn_matches(const url_t *u) {
	return sock_open && sock.connected && conn_reusable &&
		conn_host[0] &&
		!strcmp(conn_host, u->host) &&
		conn_port == url_port(u) &&
		conn_secure == (u->scheme == URL_SCHEME_HTTPS);
}

// A private copy of each Z_PORT_DATA payload.
//
// See the handler: the payload cannot be read after the ack, and the
// ack has to happen before processing. Sized above net's SOCK_CHUNK,
// which is 4096 -- an oversized payload is reported and dropped
// rather than overrunning this, so the two going out of step is
// visible rather than fatal.
// The DEFLATE window, for `Content-Encoding: gzip`.
//
// 32KB, and the largest single buffer in this app. It is here rather
// than inside http.c because not every user of that parser wants to
// spend it -- see zinflate.h.
//
// It pays for itself immediately: en.wikipedia.org's front page is
// 258KB of HTML and about a fifth of that gzipped, against a body
// transfer that is the largest cost of a page load on this machine.
static uint8_t inflate_window[Z_INFLATE_WINDOW];


static uint8_t port_rx[8192];

// Z_PORT_DATA messages and bytes seen since this connection opened.
// Declared out here rather than beside the TLS state because the
// handler that maintains them is not conditional on TLS.
static uint32_t port_msgs;
static uint32_t port_bytes;

static http_ctx_t http;
static http_response_t last_res;
static bool have_res;
static int redirects;
static url_t fetch_url;			// what the current request is for
static bool fetch_is_head;

#if WEB_TLS

#define ROOTS_PATH  "/web/roots.der"

// One TLS context, static because it holds a full record buffer and
// there is one connection in the system.
static tls_ctx_t tls;

// -- the root store --
//
// A single file of concatenated DER certificates:
//
//   for f in /etc/ssl/certs/*.pem
//   do openssl x509 -in "$f" -outform DER
//   done > roots.der
//
// INDEXED ON FIRST USE, then looked up from memory.
//
// The first version scanned the file linearly on every handshake,
// with two fs_seek() calls and two reads per certificate, parsing
// each until a subject matched. That was noted at the time as
// "deferred until there is a measurement to justify a format". The
// measurement arrived: against en.wikipedia.org the handshake
// finished only AFTER the server had given up and closed the
// connection -- net logged "session ended" before web logged "tls:
// up". A few hundred kilobytes off an SPI SD card, per
// connection, is seconds.
//
// The index holds a hash of each subject DN and where that
// certificate lives, so a lookup is a memory scan plus one seek and
// one read. The hash is only a FILTER: the DN is still compared byte
// for byte after parsing, so a collision costs a wasted read rather
// than the wrong trust anchor.
#define ROOT_MAX 320

static uint8_t root_buf[8192];

static struct {
	uint32_t	off;
	uint32_t	len;
	uint8_t		dn[8];			// first 8 bytes of SHA-256(subject DN)
} root_idx[ROOT_MAX];

static int root_n;
static bool root_indexed;

static void root_dn_hash(const der_t *dn, uint8_t out[8]) {
	uint8_t full[32];
	z_sha256(full, dn->p, dn->len);
	memcpy(out, full, 8);
}

// Reads the certificate at `off` into root_buf, returning its total
// length or 0 at end of file. `pos` tracks where the handle actually
// is, so a sequential walk needs no seeks at all -- two per
// certificate was most of the old cost.
static uint32_t root_read_at(int fd, uint32_t off, uint32_t *pos) {

	uint8_t hdr[4];
	uint32_t clen, total;

	if (*pos != off) {
		if (fs_seek(fd, off) < 0) return 0;
		*pos = off;
	}

	if (fs_read_chunk(fd, (char *)hdr, 4) != 4) return 0;
	*pos += 4;

	// Every certificate is a DER SEQUENCE, and at these sizes the
	// length is always the two-byte long form.
	if (hdr[0] != 0x30 || hdr[1] != 0x82) return 0;

	clen = ((uint32_t)hdr[2] << 8) | hdr[3];
	total = clen + 4;

	if (total > sizeof(root_buf)) return 0;

	memcpy(root_buf, hdr, 4);
	if (fs_read_chunk(fd, (char *)root_buf + 4, (int)clen) != (int)clen)
		return 0;
	*pos += clen;

	return total;

}

// The on-card index, when the store has one.
//
// tools/mkroots.py writes a header and a sorted table of (dn_hash,
// offset, length) in front of the certificates, so this app can read
// a few kilobytes and seek, instead of parsing every certificate in
// the file to find one.
//
// That cost 1.65 SECONDS on the first HTTPS fetch of every run, off a
// SPI SD card. It is build-time work and it belongs at build
// time.
//
// A store WITHOUT the header still works -- it is read as a plain
// concatenation, exactly as before. Old cards keep working; they are
// just slower.
#define ROOT_MAGIC0 0x4F4F525Au			// "ZROO"
#define ROOT_MAGIC1 0x00005354u			// "TS\0\0"

static bool root_read_index(int fd) {

	uint8_t hdr[24];
	uint32_t m0, m1, ver, count, i;
	uint32_t t0 = z_uptime_ticks();

	if (fs_seek(fd, 0) < 0) return false;
	if (fs_read_chunk(fd, (char *)hdr, 24) != 24) return false;

	m0 = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) |
		((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
	m1 = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8) |
		((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7] << 24);

	if (m0 != ROOT_MAGIC0 || m1 != ROOT_MAGIC1) return false;

	ver = (uint32_t)hdr[8] | ((uint32_t)hdr[9] << 8) |
		((uint32_t)hdr[10] << 16) | ((uint32_t)hdr[11] << 24);
	count = (uint32_t)hdr[12] | ((uint32_t)hdr[13] << 8) |
		((uint32_t)hdr[14] << 16) | ((uint32_t)hdr[15] << 24);

	// A version this app does not know is not a store to guess at.
	// Falling back to the plain scan reads the same file correctly,
	// just slowly, which is the right failure.
	if (ver != 1 || count == 0) return false;
	if (count > ROOT_MAX) count = ROOT_MAX;

	for (i = 0; i < count; i++) {
		uint8_t e[16];
		if (fs_read_chunk(fd, (char *)e, 16) != 16) return false;
		memcpy(root_idx[i].dn, e, 8);
		root_idx[i].off = (uint32_t)e[8] | ((uint32_t)e[9] << 8) |
			((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
		root_idx[i].len = (uint32_t)e[12] | ((uint32_t)e[13] << 8) |
			((uint32_t)e[14] << 16) | ((uint32_t)e[15] << 24);
	}

	root_n = (int)count;

	printf("web: root store: %d certificates, prebuilt index, %lu ms\n",
		root_n, (unsigned long)((z_uptime_ticks() - t0) * 1000u / 732u));

	return true;

}

static void root_build_index(void) {

	int fd;
	uint32_t off = 0, pos = 0;
	uint32_t t0 = z_uptime_ticks();

	if (root_indexed) return;
	root_indexed = true;			// one attempt, success or not
	root_n = 0;

	fd = fs_open_read(ROOTS_PATH);
	if (fd < 0) {
		printf("web: no root store at %s\n", ROOTS_PATH);
		return;
	}

	// A store built by tools/mkroots.py carries its own index.
	if (root_read_index(fd)) {
		fs_close_handle(fd);
		return;
	}

	// Otherwise: a plain concatenation, parsed here. Correct, and
	// about 1.6 seconds slower.
	if (fs_seek(fd, 0) < 0) { fs_close_handle(fd); return; }

	for (;;) {

		x509_cert_t c;
		const char *e = NULL;
		uint32_t total = root_read_at(fd, off, &pos);

		if (total == 0) break;

		if (root_n < ROOT_MAX && x509_parse(root_buf, total, &c, &e)) {
			root_idx[root_n].off = off;
			root_idx[root_n].len = total;
			root_dn_hash(&c.subject, root_idx[root_n].dn);
			root_n++;
		}

		off += total;

	}

	fs_close_handle(fd);

	printf("web: root store: %d certificates indexed in %lu ms\n",
		root_n, (unsigned long)((z_uptime_ticks() - t0) * 1000u / 732u));

}

static bool web_find_root(void *user, const der_t *issuer, x509_cert_t *out) {

	int fd;
	uint8_t want[8];
	uint32_t pos = 0;
	bool found = false;

	(void)user;

	root_build_index();
	if (root_n == 0) return false;

	root_dn_hash(issuer, want);

	fd = fs_open_read(ROOTS_PATH);
	if (fd < 0) return false;

	for (int i = 0; i < root_n; i++) {

		const char *e = NULL;
		uint32_t total;

		if (memcmp(root_idx[i].dn, want, 8)) continue;

		total = root_read_at(fd, root_idx[i].off, &pos);
		if (total == 0) continue;

		if (!x509_parse(root_buf, total, out, &e)) continue;

		// The hash was only a filter; the DN itself decides.
		if (out->subject.len == issuer->len &&
			!memcmp(out->subject.p, issuer->p, issuer->len)) {
			found = true;
			break;
		}

	}

	fs_close_handle(fd);
	return found;

}

// Whether the CURRENT fetch is over TLS.
//
// DERIVED, not stored. This used to be a `tls_active` flag, and the
// flag had to be set AFTER sock_disconnect(), because that function
// cleared it while tearing down the previous connection. Nothing
// enforced that ordering, and it was in fact already broken:
// start_fetch() set the flag and then called sock_disconnect(), so
// every https fetch ran as PLAINTEXT HTTP to port 443 -- the
// connection opened, an unencrypted request went out, the server
// closed it, and it surfaced as "connection closed before any
// response" from the HTTP layer with no TLS code having run at all.
//
// A derived value cannot fall out of step with the thing it
// describes, so the class of bug goes away rather than this one
// instance of it.
static bool tls_in_use(void) {
	return fetch_url.scheme == URL_SCHEME_HTTPS;
}
static bool tls_req_sent;

// tls.c hands back ciphertext to put on the wire.
static bool tls_send_failed;

static void tls_on_send(void *user, const uint8_t *d, uint32_t n) {
	(void)user;
	if (!sock_open) { tls_send_failed = true; return; }
	// A dropped record is not a dropped keystroke. Losing part of a
	// TLS record makes the connection unrecoverable in a way that
	// surfaces much later, or -- as here, for the ClientHello -- as a
	// server that simply closes on a connection it was never spoken
	// to. Swallowing the return value hid exactly that.
	if (z_port_send(&sock, d, n) != Z_OK) tls_send_failed = true;
}

// ...and plaintext coming the other way, which is just HTTP.
static void tls_on_data(void *user, const uint8_t *d, uint32_t n) {
	(void)user;
	if (state == W_RECEIVING) http_feed(&http, (const char *)d, n);
}

// Bytes seen from the peer before the handshake completed. Reported
// on failure: "closed after 0 bytes" and "closed after 4000 bytes"
// are completely different problems and the error text alone cannot
// tell them apart.
static uint32_t tls_rx_bytes;

// When the handshake started, in z_uptime_ticks() (~732Hz).
//
// A TLS 1.3 client says NOTHING between ClientHello and Finished, so
// a server waiting on a flight that never completes just sits there
// -- and so did this app, for over a minute, until the server's own
// timeout closed the connection. Failing here first turns that into
// a report with numbers in it instead of a hang.
static uint32_t tls_start_ticks;

// When the handshake began, for reporting how long it took. The
// answer matters: a server that closes before the client has finished
// verifying is not a protocol failure, it is a speed one.
static uint32_t tls_t0;


// ~15 seconds. Long enough for a slow path plus a chain verification
// on this CPU, short enough not to look like a lockup.
#define TLS_HANDSHAKE_TIMEOUT_TICKS  (15u * 732u)

#endif

// Everything above the socket goes through these two, so that the
// HTTP layer never learns whether it is talking through TLS. That is
// the same reason tls.c takes callbacks rather than a socket: one
// path, exercised either way.
static bool transport_send(const uint8_t *d, uint32_t n) {
#if WEB_TLS
	if (tls_in_use()) return tls_write(&tls, d, n);
#endif
	return z_port_send(&sock, d, n) == Z_OK;
}

static void transport_recv(const uint8_t *d, uint32_t n) {
#if WEB_TLS
	if (tls_in_use()) {
		uint32_t t0 = web_ms();
		if (!tls_established(&tls)) tls_rx_bytes += n;
		tls_feed(&tls, d, n);
		tm.crypto_ms += web_ms() - t0;
		return;
	}
#endif
	if (state == W_RECEIVING) http_feed(&http, (const char *)d, n);
}

// -- the fetch service ---------------------------------------------
//
// A request from another process. One at a time, because `net` has
// one TCB; a second is queued rather than refused, since a client
// that got "busy" could only retry in a loop doing the same thing
// worse.
#define SVC_QUEUE 4

typedef struct {
	bool		used;
	uint32_t	pid;
	uint32_t	tag;
	char		url[URL_MAX];
	char		accept[64];
	bool		head;
	uint32_t	handle;
} svc_req_t;

static svc_req_t svc_q[SVC_QUEUE];
static int svc_active = -1;			// index into svc_q, or -1 for our own
static uint32_t svc_next_handle = 1;

static zstream_producer_t svc_stream;
static bool svc_streaming;
static uint32_t svc_stream_off;
static uint32_t svc_stream_handle;

// -- forward declarations ------------------------------------------

static void repaint(void);
static void img_overlay(uint32_t block, layout_img_t *o);
static void start_fetch(const url_t *u, bool push_history);
static bool from_redirect;
static void fetch_failed(const char *why);
static void handle_port_msg(z_msg_t *msg);

// -- drawing -------------------------------------------------------

static int view_x, view_y, view_w, view_h;

// The content area in SCREEN coordinates, refreshed on every
// relayout(). z_win_* drawing calls take content-relative
// coordinates; z_fb_* calls -- which is what layout.c uses, because
// it has to be usable from tests/render.c without a window manager --
// take screen ones. Keeping the rect here means the conversion is one
// addition at each of the three places that need it, rather than
// something to get right per call.
static z_clip_t crect;

static layout_hit_t hits[LAYOUT_MAX_HITS];
static int nhits;
static int sel_link = -1;

static void relayout(void) {

	int cw = z_win_content_w(&win);
	int ch = z_win_content_h(&win);
	int sb = 10;

	z_win_content_rect(&win, &crect);

	view_x = 2;
	view_y = BAR_H;
	view_w = cw - 4 - sb;
	view_h = ch - BAR_H;

	if (view_w < 32) view_w = 32;
	if (view_h < 16) view_h = 16;

	memset(&cfg, 0, sizeof(cfg));
	// A SCREEN x-origin, not a content-relative one. layout.h explains
	// why that choice is made once here rather than at every call.
	cfg.x = crect.x0 + view_x;
	cfg.width = view_w;
	cfg.body = &z_font_5x8;
	cfg.img = img_overlay;
	cfg.head = &z_font_6x12;
	cfg.links_live = (state == W_READY);

	z_scrollbar_set_geom(&sbar, cw - sb, BAR_H, view_h);

}

// The location strip. Shows the URL while a page is up and the
// status while one is loading -- one line, because a second would
// cost 9 of the 480 pixels available and say nothing new.
// A button: a frame, and an arrow pointing the way it goes.
//
// The shape comes from toolbar_arrow_px() rather than being computed
// here, because the first version of this computed it inline and drew
// nothing at all -- the buttons appeared as empty boxes and the code
// read as though it should work.
//
// A DISABLED button still shows its arrow, stippled. It used to show
// an empty frame, on the theory that greying is unavailable on one
// bit; but an empty frame does not say "you cannot go back", it says
// "this button is broken", which is precisely how it was reported.
static void draw_arrow(int x, int y, int h, bool left, bool live) {

	z_clip_t c;
	int dx, dy;

	z_win_content_rect(&win, &c);

	// The frame, as four one-pixel fills. Drawn with the blitter
	// rather than the line rasterizer for the same single-engine
	// reason as the URL field -- see zedit.h.
	z_fb_hw_fill_rect(c.x0 + x, c.y0 + y, TB_BTN_W, 1, 1);
	z_fb_hw_fill_rect(c.x0 + x, c.y0 + y + h - 1, TB_BTN_W, 1, 1);
	z_fb_hw_fill_rect(c.x0 + x, c.y0 + y, 1, h, 1);
	z_fb_hw_fill_rect(c.x0 + x + TB_BTN_W - 1, c.y0 + y, 1, h, 1);

	// The arrow, a pixel at a time. It is at most a few dozen pixels
	// and this runs once per bar repaint, so a run-length version
	// would be more code for time nobody can measure.
	for (dy = 0; dy < h; dy++) {
		for (dx = 0; dx < TB_BTN_W; dx++) {
			if (!toolbar_arrow_px(dx, dy, TB_BTN_W, h, left)) continue;
			if (!live && !toolbar_stipple(dx, dy)) continue;
			z_fb_hw_fill_rect(c.x0 + x + dx, c.y0 + y + dy, 1, 1, 1);
		}
	}

}

static void draw_bar(void) {

	int cw = z_win_content_w(&win);
	toolbar_t tb;

	toolbar_geom(cw, &tb);

	z_win_fill_rect(&win, 0, 0, cw, BAR_H, 0);

	// While a page is loading the field shows progress; the rest of
	// the time it shows the URL, or whatever is being typed into it.
	if (!url_editing) {
		if (state == W_READY || state == W_IDLE)
			z_edit_set(&url_edit, cur_url_text);
		else
			z_edit_set(&url_edit, status);
	}

	url_edit.focus = url_focus;
	z_edit_draw(&win, &url_edit, tb.field_x, 1, tb.field_w, BAR_H - 3,
		&z_font_5x8);

	if (tb.buttons) {
		draw_arrow(tb.back_x, 1, BAR_H - 3, true, hist_at > 0);
		draw_arrow(tb.fwd_x, 1, BAR_H - 3, false, hist_at + 1 < hist_n);
	}

	z_win_hw_line(&win, 0, BAR_H - 1, cw - 1, BAR_H - 1, 1);

}

// What layout.c should draw in place of a placeholder box.
//
// Filled in per block, so a page with twenty images draws boxes for
// nineteen of them and the picture for the one that was clicked.
static void img_overlay(uint32_t block, layout_img_t *o) {

	o->bits = NULL;
	o->caption = NULL;

	if (img_status[0] && block == img_block)
		o->caption = img_status;

	if (!img_have || block != img_block) return;

	o->bits = img_bits;
	o->wpl = IMG_WPL;
	o->w = img_w;
	o->h = img_h;
	o->caption = NULL;

}

static void draw_body(void) {

	static html_line_t blocks[PAGE_FETCH_MAX];
	uint32_t n, i;
	int y = view_y;
	int prev_kind = -1;
	bool first = true;

	z_win_fill_rect(&win, 0, view_y, z_win_content_w(&win), view_h, 0);

	nhits = 0;

	if (page_blocks(&pg) == 0) return;

	n = page_fetch(&pg, top.block, PAGE_FETCH_MAX, blocks);

	for (i = 0; i < n && y < view_y + view_h; i++) {

		const html_line_t *l = &blocks[i];
		int lh = layout_line_height(l, &cfg);
		int from = first ? (int)top.sub : 0;
		int gap = first ? 0 : layout_gap_before(l, &cfg);
		int avail, drew;

		// Consecutive blocks of the same kind are one structure -- a
		// list, a table, a <pre> -- so the inter-block gap is
		// suppressed between them. Without this a ten-row table is
		// spread over two screens for no reason.
		if ((int)l->kind == prev_kind &&
			(l->kind == HTML_LIST || l->kind == HTML_TABLE ||
			 l->kind == HTML_PRE))
			gap = 0;

		y += gap;

		avail = (view_y + view_h - y) / lh;
		if (avail <= 0) break;

		drew = layout_draw(l, &cfg, crect.y0 + y, from, avail,
			&crect, (uint16_t)(top.block + i),
			hits, &nhits, LAYOUT_MAX_HITS);

		y += drew * lh;
		prev_kind = (int)l->kind;
		first = false;

	}

	// The selected link, boxed. Keyboard link selection needs a
	// visible cursor and underline alone is not one -- every link has
	// that already.
	if (sel_link >= 0 && sel_link < nhits) {
		const layout_hit_t *h = &hits[sel_link];
		z_fb_hw_box(h->x - 1, h->y - 1, h->x + h->w, h->y + h->h,
			1, &crect);
	}

}

static void draw_scrollbar(void) {

	// Over BLOCKS, not display lines -- page.h says why. A line-based
	// scrollbar would have to re-count every block in the document on
	// every window resize.
	int32_t total = (int32_t)page_blocks(&pg);
	int32_t page_span = 8;

	if (total < 1) total = 1;

	z_scrollbar_set_range(&sbar, total, page_span);
	z_scrollbar_set_value(&sbar, (int32_t)top.block);
	z_scrollbar_draw(&sbar, true);

}

// Draws the window. Does NOT acknowledge a redraw.
//
// z_win_redraw_done() is only for redraws the WM ASKED for: it blocks
// wm, per window, back-to-front, before letting anything in front
// draw its own content (zwin.h). Sending it for self-initiated
// repaints -- which is what this app does on every scroll, every
// status change and every fetch transition -- leaves wm's ordering
// accounting wrong, and the visible symptom is a DIALOG that takes
// seconds to appear, because the dialog is a window in front of this
// one and wm is waiting on an ordering that will never resolve.
static void repaint(void) {

	relayout();
	draw_bar();

	// The page is left alone while an image loads.
	//
	// draw_body() clears the whole content area before redrawing it,
	// so a repaint driven by a changing status line -- resolving,
	// connecting, handshaking, receiving -- blanked and redrew the
	// page several times over the course of one image fetch. On this
	// CPU that is visible as flicker, and the page did not change:
	// only the status did.
	//
	// The one body redraw an image DOES need -- to put "Loading
	// image..." in its box -- is done explicitly by load_image()
	// before this takes effect, and another follows when the picture
	// arrives.
	if (!fetching_image) draw_body();

	draw_scrollbar();

}

// A full repaint including the body, for the moments an image fetch
// genuinely changes what is on the page.
static void repaint_all(void) {
	relayout();
	draw_bar();
	draw_body();
	draw_scrollbar();
}

// The Z_WM_REDRAW path, and the only place the acknowledgement
// belongs.
static void repaint_for_wm(void) {
	repaint();
	z_win_redraw_done(&win);
}

// -- scrolling -----------------------------------------------------

static int lines_per_screen(void) {
	int lh = z_font_5x8.h + 1;
	int n = view_h / lh;
	return n > 0 ? n : 1;
}

// Display lines occupied by an image block at the edge of the view.
//
// `dir` is the direction of travel: scrolling down asks about the
// block at the top, scrolling up about the one just above it, since
// that is the one about to come into view.
//
// Returns 0 when the block in question is not an image, which leaves
// scrolling exactly as it was for text.
static int32_t img_block_lines(int32_t dir) {

	static html_line_t l;
	uint32_t b = top.block;

	if (dir < 0) {
		if (b == 0) return 0;
		b--;
	}

	if (page_fetch(&pg, b, 1, &l) != 1) return 0;
	if (l.kind != HTML_IMAGE) return 0;

	return layout_count(&l, &cfg);

}

static void scroll_by(int32_t lines) {

	// A single-line step onto or off an image moves the WHOLE image.
	//
	// Stepping through a picture a text line at a time is wrong for
	// the same reason stepping through a paragraph a pixel at a time
	// would be: the line is not the unit the content is made of.
	if (lines == 1 || lines == -1) {
		int32_t n = img_block_lines(lines);
		if (n > 1) lines = (lines > 0) ? n : -n;
	}

	if (page_advance(&pg, &cfg, &top, lines) != 0) {
		sel_link = -1;
		repaint();
	}
}

static void scroll_to_top(void) {
	top.block = 0;
	top.sub = 0;
	sel_link = -1;
	repaint();
}

static void scroll_to_end(void) {
	if (page_blocks(&pg) == 0) return;
	top.block = page_blocks(&pg) - 1;
	top.sub = 0;
	page_advance(&pg, &cfg, &top, -(lines_per_screen() - 1));
	sel_link = -1;
	repaint();
}

// -- spool ---------------------------------------------------------

static void spool_close(void) {
	if (spool_rd >= 0) { fs_close_handle(spool_rd); spool_rd = -1; }
	if (spool_wr >= 0) { fs_close_handle(spool_wr); spool_wr = -1; }
}

static bool spool_begin(void) {

	// An image gets its OWN write handle and leaves the page's spool
	// completely alone.
	//
	// The first version reused spool_wr, so spool_close() below shut
	// spool_rd -- the handle page.c fetches blocks through. The page
	// went blank the instant an image was clicked, which looked like
	// the image had replaced the document.
	if (fetching_image) {
		snprintf(img_spool_path, sizeof(img_spool_path), "%s.img",
			spool_path);
		if (img_wr >= 0) { fs_close_handle(img_wr); img_wr = -1; }
		fs_unlink(img_spool_path);
		img_wr = fs_open_write(img_spool_path);
		if (img_wr < 0) {
			fetch_failed("cannot open the image spool for writing");
			return false;
		}
		spool_wlen = 0;
		return true;
	}

	spool_close();

	// The directory may not exist on a fresh card. fs_open_write()
	// does not create parents, so a missing /web is reported as a
	// plain "cannot open" -- which is true and unhelpful, hence the
	// specific message below.
	spool_wr = fs_open_write(spool_path);
	if (spool_wr < 0) {
		fetch_failed("cannot open the spool for writing");
		return false;
	}

	spool_len = 0;
	spool_wlen = 0;
	return true;

}

static bool spool_finish(void) {

	if (!spool_flush()) {
		fetch_failed("write to the spool failed -- card full?");
		return false;
	}

	// An image closes its own handle; the page's spool is untouched
	// and still open for reading.
	if (fetching_image) {
		if (img_wr >= 0) { fs_close_handle(img_wr); img_wr = -1; }
		return true;
	}

	if (spool_wr >= 0) { fs_close_handle(spool_wr); spool_wr = -1; }

	spool_rd = fs_open_read(spool_path);
	if (spool_rd < 0) {
		fetch_failed("cannot re-open the spool for reading");
		return false;
	}

	spool_pos = 0;
	return true;

}

// -- HTTP callbacks ------------------------------------------------

static void on_headers(void *user, const http_response_t *r) {

	(void)user;

	last_res = *r;
	have_res = true;

	// The server's half of the bargain. It may close whatever we
	// asked for, and a response that is not self-delimiting cannot
	// be followed by anything either -- http.c decides both.
	conn_reusable = r->keep_alive;

	// A redirect's body is not worth spooling -- it is a courtesy
	// page nobody will see, and the next request is about to reuse
	// the same spool file.
	if (r->status >= 300 && r->status < 400 && r->has_location) return;

	snprintf(status, sizeof(status), "receiving %s...",
		r->content_type[0] ? r->content_type : "response");

}

static void on_body(void *user, const char *data, uint32_t len) {

	(void)user;

	// The ACTIVE handle, which for an image is img_wr.
	//
	// Testing spool_wr alone dropped every byte of an image: spool_wr
	// is -1 throughout an image fetch by design, so the spool file
	// was created, written to zero times, and decoded as "empty
	// file".
	if ((fetching_image ? img_wr : spool_wr) < 0 || len == 0) return;

	if (last_res.status >= 300 && last_res.status < 400 &&
		last_res.has_location) return;

	{
		uint32_t off = 0;

		while (off < len) {

			uint32_t take = SPOOL_BUF - spool_wlen;

			if (take > len - off) take = len - off;
			memcpy(spool_wbuf + spool_wlen, data + off, take);
			spool_wlen += take;
			off += take;

			if (spool_wlen == SPOOL_BUF && !spool_flush()) {
				fetch_failed("write to the spool failed -- card full?");
				return;
			}

		}
	}

	spool_len += len;
	tm.body_bytes += len;

	// Repainting per chunk would be a repaint every 536 bytes. Once
	// every 16KB is often enough to show progress and rare enough not
	// to be the reason the transfer is slow.
	if ((spool_len & 0x3FFF) < len) {
		snprintf(status, sizeof(status), "receiving... %lu KB",
			(unsigned long)(spool_len / 1024));
		draw_bar();
	}

}

// -- transport -----------------------------------------------------

static void sock_disconnect(void) {
#if WEB_TLS
	if (tls_in_use() && sock_open && tls_established(&tls)) tls_close(&tls);
	tls_req_sent = false;
#endif
	if (sock_open) {
		z_port_close(&sock);
		sock_open = false;
	}
	memset(&sock, 0, sizeof(sock));
}

static void fetch_failed(const char *why) {

	sock_disconnect();
	spool_close();

	// An image that failed leaves the PAGE alone -- it is still
	// there, still readable, and the reader only asked for a
	// picture. The box says what went wrong; the document does not
	// become an error screen because one image was unreachable.
	if (fetching_image) {
		fetching_image = false;
		img_loading = false;
		if (img_wr >= 0) { fs_close_handle(img_wr); img_wr = -1; }
		if (!img_status[0])
			snprintf(img_status, sizeof(img_status), "%s", why);
		state = W_READY;
		printf("web: image fetch failed: %s\n", why);
		repaint_all();
		return;
	}

	state = W_ERROR;
	snprintf(status, sizeof(status), "%s", why);

	// On the console as well. The window shows only the LAST failure,
	// and a run of retries where each one fails differently is
	// exactly the case where that is not enough to work from.
	printf("web: fetch failed: %s\n", why);

	// What this side actually received, to sit next to net's
	// "relayed" figure.
	//
	// If the two agree, no message was lost and the corruption is in
	// ordering or in this app's handling; if web's total is short,
	// bytes went missing between the two processes. Those are
	// different bugs and the error text cannot tell them apart.
	if (port_msgs)
		printf("web: received %lu msgs, %lu bytes on this connection\n",
			(unsigned long)port_msgs, (unsigned long)port_bytes);

	// A queued service request fails with the same message its own
	// client can act on.
	if (svc_active >= 0) {
		svc_req_t *q = &svc_q[svc_active];
		z_obj_t reply = z_obj_map(2);
		z_map_set(&reply, "ok", z_obj_uint32(0));
		z_map_set(&reply, "error", z_obj_str(why));
		z_msg_new_send(q->pid, Z_WEB_FETCH_REPLY, q->tag, reply);
		q->used = false;
		svc_active = -1;
	}

	repaint();

}

// Sends the request once the socket is up.
static void send_request(void) {

	static char req[2048];
	uint32_t n;

	n = http_build_request(req, sizeof(req),
		fetch_is_head ? "HEAD" : "GET", &fetch_url, NULL, true, true);

	if (n == 0) { fetch_failed("could not build the request"); return; }

	{
		char target[URL_PATH_MAX * 2];
		url_request_target(&fetch_url, target, sizeof(target));
		printf("web: req %s %s host=%s\n",
			fetch_is_head ? "HEAD" : "GET", target, fetch_url.host);
	}

	// The origin this socket now belongs to. Recorded here rather
	// than at connect time so it covers the reuse path too.
	snprintf(conn_host, sizeof(conn_host), "%s", fetch_url.host);
	conn_port = url_port(&fetch_url);
	conn_secure = (fetch_url.scheme == URL_SCHEME_HTTPS);
	conn_reusable = false;			// until the response says otherwise

	http_init(&http, fetch_is_head, on_headers, on_body, NULL);
	http_set_inflate(&http, inflate_window);
	have_res = false;

	if (!transport_send((const uint8_t *)req, n)) {
		fetch_failed("could not send the request");
		return;
	}

	state = W_RECEIVING;
	snprintf(status, sizeof(status), "requesting %s...", fetch_url.host);
	repaint();

}

// The response is complete. Either follow a redirect or hand over
// what arrived.
static void response_done(void) {

	sock_disconnect();

	if (!have_res) { fetch_failed("no response"); return; }

	printf("web: http %u\n", (unsigned)last_res.status);
	if (last_res.has_location)
		printf("web: loc %s\n", last_res.location);

	// -- redirects --
	if (last_res.status >= 300 && last_res.status < 400 &&
		last_res.has_location) {

		url_t dest;

		if (++redirects > MAX_REDIRECTS) {
			fetch_failed("too many redirects");
			return;
		}

		if (!url_resolve_str(&fetch_url, last_res.location, &dest)) {
			fetch_failed("redirect to an unparseable URL");
			return;
		}

		{
			char dbuf[URL_MAX];
			url_format(&dest, dbuf, sizeof(dbuf), false);
			printf("web: dest %s\n", dbuf);
		}

		// A redirect to where we already are is a loop, and it will
		// never resolve itself. Catching it here names the problem;
		// letting the counter run to eight reports "too many
		// redirects" after eight full TLS handshakes and says
		// nothing about why.
		if (url_same_origin(&dest, &fetch_url) &&
			!strcmp(dest.path, fetch_url.path) &&
			dest.has_query == fetch_url.has_query &&
			!strcmp(dest.query, fetch_url.query)) {
			fetch_failed("the server redirected this page to itself");
			return;
		}

		// An https page must never be redirected down to http. The
		// user asked for a secure connection and the answer to "the
		// server would rather you did not have one" is no.
		if (fetch_url.scheme == URL_SCHEME_HTTPS &&
			dest.scheme != URL_SCHEME_HTTPS) {
			fetch_failed("refused a redirect from https to http");
			return;
		}

		fetch_url = dest;
		url_format(&fetch_url, cur_url_text, sizeof(cur_url_text), true);
		snprintf(status, sizeof(status), "redirected to %s", cur_url_text);
		repaint();
		from_redirect = true;
		start_fetch(&fetch_url, false);
		return;

	}

	tm.t_body = web_ms() - tm.t_fetch - tm.t_dns - tm.t_connect - tm.t_ready;

	if (!spool_finish()) return;

	// An image finishes here: decode it into the placeholder box and
	// repaint. The page's URL, index and scroll position are all
	// untouched, which is the whole point of doing it this way.
	if (fetching_image) {

		int bw, bh;

		fetching_image = false;
		img_loading = false;
		state = W_READY;

		img_fd = fs_open_read(img_spool_path);

		img_box_size(img_block, &bw, &bh);
		img_have = img_decode_spooled(bw, bh);

		// The box now follows the picture, so there is no leftover
		// between the image and the text below it.
		if (img_have) {
			cfg.img_src = img_src;
			cfg.img_dw = img_w;
			cfg.img_dh = img_h;
		}

		if (img_fd >= 0) { fs_close_handle(img_fd); img_fd = -1; }
		fs_unlink(img_spool_path);

		if (!img_have && !img_status[0])
			snprintf(img_status, sizeof(img_status), "could not decode");

		// The body, explicitly: this is the moment the page changes.
		repaint_all();
		return;

	}

	cur_url = fetch_url;
	url_format(&cur_url, cur_url_text, sizeof(cur_url_text), true);

	// A service request wants the headers and a handle, not a render.
	if (svc_active >= 0) {

		svc_req_t *q = &svc_q[svc_active];
		z_obj_t reply = z_obj_map(7);

		q->handle = svc_next_handle++;

		z_map_set(&reply, "ok", z_obj_uint32(1));
		z_map_set(&reply, "status", z_obj_uint32(last_res.status));
		z_map_set(&reply, "content_type", z_obj_str(last_res.content_type));
		z_map_set(&reply, "charset", z_obj_str(last_res.charset));
		z_map_set(&reply, "length", z_obj_uint32(spool_len));
		z_map_set(&reply, "url", z_obj_str(cur_url_text));
		z_map_set(&reply, "handle", z_obj_uint32(q->handle));
		z_msg_new_send(q->pid, Z_WEB_FETCH_REPLY, q->tag, reply);

		state = W_READY;
		snprintf(status, sizeof(status), "served %lu bytes to pid %lu",
			(unsigned long)spool_len, (unsigned long)q->pid);
		repaint();
		return;

	}

	// -- our own page --

	page_reset(&pg);
	page_set_charset(&pg,
		(!strcmp(last_res.charset, "iso-8859-1") ||
		 !strcmp(last_res.charset, "windows-1252") ||
		 !strcmp(last_res.charset, "latin1"))
			? HTML_CS_CP1252 : HTML_CS_UTF8);
	page_set_size(&pg, spool_len, true);

	top.block = 0;
	top.sub = 0;
	sel_link = -1;

	state = W_INDEXING;
	snprintf(status, sizeof(status), "reading %lu bytes...",
		(unsigned long)spool_len);
	repaint();

}

// -- loading an image in place -------------------------------------
//
// The page stays exactly as it is: the image goes to its OWN spool
// file and is decoded into img_bits, and only the placeholder box
// changes.
//
// This reuses start_fetch() rather than running a second fetch path
// beside it. Everything that is hard -- DNS, TLS, redirects, keep
// alive, the receive loop -- is the same, and the only differences
// are where the bytes land and what happens when they stop. A
// parallel path would have been a second place for every one of those
// bugs to live.
//
// It also inherits connection reuse, so clicking an image on the page
// you just loaded usually costs no handshake at all.
static void load_image(uint32_t block, const html_line_t *l) {

	url_t dest;

	if (state == W_RECEIVING || state == W_IMG) return;	// one at a time

	if (!l->nlinks) {
		snprintf(img_status, sizeof(img_status), "this image has no address");
		img_block = block;
		repaint();
		return;
	}

	if (!url_resolve_str(&cur_url, l->links[0], &dest)) {
		snprintf(img_status, sizeof(img_status), "bad image address");
		img_block = block;
		repaint();
		return;
	}

	// Set before anything blocks, so the box says what is happening
	// rather than sitting inert through a lookup and a fetch.
	img_forget();
	img_loading = true;
	img_block = block;
	snprintf(img_src, sizeof(img_src), "%s", l->links[0]);
	snprintf(img_status, sizeof(img_status), "Loading image...");

	// Before fetching_image is set, so the body is drawn once with
	// the caption in it. Everything after this leaves the page alone.
	repaint_all();

	// Tells spool_begin(), repaint() and the completion path that
	// this fetch is an image. The page's own URL, index and pixels
	// are all left untouched.
	fetching_image = true;

	start_fetch(&dest, false);

}

static void start_fetch(const url_t *u, bool push_history) {

	uint32_t ip;
	z_obj_t arg;

	if (!url_is_network(u)) {
		if (u->scheme == URL_SCHEME_OTHER) {
			char msg[160];
			snprintf(msg, sizeof(msg), "cannot follow a %s: link",
				u->scheme_text);
			fetch_failed(msg);
		} else {
			fetch_failed("not a http:// or https:// URL");
		}
		return;
	}

#if !WEB_TLS
	if (u->scheme == URL_SCHEME_HTTPS) {

		char msg[URL_MAX + 128];
		char where[URL_MAX];

		url_format(u, where, sizeof(where), false);

		// Two different situations, and saying the same thing for
		// both is actively misleading.
		//
		// Asking for an https URL and being told this build has no
		// TLS is a complete explanation. Asking for an HTTP URL and
		// being told the same thing is not: nothing the person did
		// mentioned https, and the sentence reads as though the
		// browser refused their request rather than the server's
		// redirect. Almost every site on the web now answers port 80
		// with exactly this redirect, so the misleading half is the
		// COMMON case, not the rare one.
		if (from_redirect)
			snprintf(msg, sizeof(msg),
				"the site redirected to %s -- https needs TLS, which "
				"this build does not have", where);
		else
			snprintf(msg, sizeof(msg),
				"https needs TLS, which this build does not have");

		fetch_failed(msg);
		return;

	}
#endif

	if (!net_pid && !z_pid_lookup("net0", &net_pid)) {
		fetch_failed("net is not running -- try `run net`");
		return;
	}

	fetch_url = *u;

	// An image fetch does not change where the reader is. Writing the
	// image's address here put it in the URL bar, which said the
	// browser had navigated to the picture -- and after the fetch the
	// page it belonged to was gone from the bar as well as the
	// screen.
	if (!fetching_image)
		url_format(&fetch_url, cur_url_text, sizeof(cur_url_text), true);


	if (push_history) {
		redirects = 0;
		from_redirect = false;
		if (hist_at + 1 < HIST_MAX) {
			hist_at++;
			hist_n = hist_at + 1;
			snprintf(hist[hist_at], URL_MAX, "%s", cur_url_text);
		}
	}

	if (port_msgs)
		printf("web: prev conn: %lu msgs, %lu bytes\n",
			(unsigned long)port_msgs, (unsigned long)port_bytes);
	port_msgs = 0;
	port_bytes = 0;

	// An image is never a redirect continuation. The flag is page
	// state and left set from the last navigation, which made the
	// console call an image fetch "(redirect)".
	if (fetching_image) from_redirect = false;

	if (fetching_image) {
		char where[URL_MAX];
		url_format(&fetch_url, where, sizeof(where), true);
		printf("web: image fetch %s\n", where);
	} else
	printf("web: fetch %s%s\n", cur_url_text,
		from_redirect ? " (redirect)" : "");

	memset(&tm, 0, sizeof(tm));
	tm.t_fetch = web_ms();

	// Reset the HTTP parser HERE, at the start of the fetch, not in
	// send_request().
	//
	// send_request() is only reached once a TLS handshake completes,
	// so between connections the context kept whatever state the LAST
	// response left it in -- ST_DONE. The first byte to arrive on the
	// next connection then found http_done() already true, and
	// response_done() ran against the previous response's headers.
	//
	// The visible result was a page that appeared to redirect to
	// itself: the 301 and the Location being acted on were connection
	// one's, replayed against connection two's URL.
	http_init(&http, false, on_headers, on_body, NULL);
	have_res = false;

	// Reuse the open connection when this fetch is to the same
	// origin and the last response left it usable.
	//
	// Everything below -- teardown, DNS, connect, handshake -- is
	// skipped, and the request goes out on the socket that is
	// already up.
	if (!fetch_retrying && conn_matches(&fetch_url)) {

		printf("web: reusing connection to %s\n", fetch_url.host);

		if (!spool_begin()) return;

		fetch_reused = true;
		conn_idle_deadline = 0;
		state = W_RECEIVING;
		tm.t_dns = 0;
		tm.t_connect = 0;
		tm.t_ready = 0;
		send_request();
		return;

	}

	fetch_reused = false;
	fetch_retrying = false;

	// A block number means nothing once a different document is
	// loaded, so a held image would reappear over an unrelated
	// paragraph. Not cleared for an image fetch, which is not a new
	// document.
	if (!fetching_image) img_forget();

	sock_disconnect();
	conn_forget();

	// Drain anything left over from the connection just torn down,
	// BEFORE starting a new one.
	//
	// z_dns_resolve() below blocks, and any Z_PORT_DATA still queued
	// would sit unread across it -- in the same mailbox the NEXT
	// connection's data has to arrive in. The new connection could
	// then fail its very first delivery having never been slow at
	// all, which is exactly what "relayed 0" looked like.
	//
	// Messages that are NOT from the old connection are HANDLED, not
	// thrown away. The first version discarded everything it read,
	// so it could swallow a Z_WM_REDRAW -- and wm blocks waiting for
	// that acknowledgement, which re-introduced the very stall that
	// acking redraws properly had just fixed.
	{
		z_msg_t stale;
		int drained = 0;

		while (drained < 64 && z_msg_read(&stale) == Z_OK) {

			switch (stale.subject) {

			case Z_PORT_DATA:
				// From the connection that is gone: ack it so net can
				// free the slot, and drop the payload.
				z_port_send_ack(&stale);
				break;

			case Z_WM_REDRAW:
				z_win_apply_redraw(&win, stale.obj.type == Z_UINT32
					? stale.obj.val.uint32 : 0);
				repaint_for_wm();
				break;

			case Z_WM_WINDOW_RESIZED:
				if (z_win_apply_resized(&win, &stale.obj)) {
					top.sub = 0;
					sel_link = -1;
					repaint();
				}
				break;

			default:
				break;

			}

			drained++;

		}
	}

	state = W_RESOLVING;
	snprintf(status, sizeof(status), "resolving %s...", fetch_url.host);
	repaint();

	// z_dns_resolve() BLOCKS. It is bounded (zdns.h) but this process
	// does not read messages while it runs, so the status line above
	// is drawn FIRST -- the window will not repaint again until this
	// returns. Same constraint, and the same mitigation, as
	// z_conn_prepare() documents for `term`.
	tm.t_dns = web_ms();

	if (!z_resolve_host(fetch_url.host, &ip, NULL, 0)) {
		fetch_failed("could not resolve that host");
		return;
	}

	tm.t_dns = web_ms() - tm.t_dns;

	if (!spool_begin()) return;

	state = W_CONNECTING;
	snprintf(status, sizeof(status), "connecting to %s...", fetch_url.host);
	repaint();

	// The port handshake is done BY HAND rather than through
	// z_port_connect_arg_timeout(), which blocks until net answers.
	// net does not answer until a real TCP handshake resolves, and
	// that is up to tcp.c's whole retry budget (~31.5s). Blocking
	// that long means no repaints and wm reporting a missed redraw
	// ack -- exactly the bug term's connect path had to grow a custom
	// message pump to avoid. Sending the CONNECT and handling
	// CONNECTED/REFUSED in the main loop avoids needing one.
	arg = z_obj_map(2);
	z_map_set(&arg, "ip", z_obj_uint32(ip));
	z_map_set(&arg, "port", z_obj_uint32(url_port(&fetch_url)));
	z_msg_new_send(net_pid, Z_PORT_CONNECT, 0, arg);

}

// -- navigation ----------------------------------------------------

static void go_to_text(const char *text) {

	url_t u;
	char buf[URL_MAX];

	// A bare "example.com" is what a person types. Treat anything
	// with no scheme and no leading slash as a hostname rather than
	// as a relative path, which is what every browser does and is the
	// difference between the URL bar being usable and not.
	if (!strstr(text, "://") && text[0] != '/' && !strchr(text, ' ')) {
		snprintf(buf, sizeof(buf), "http://%s", text);
		text = buf;
	}

	if (!url_parse(text, &u) || !url_is_network(&u)) {
		fetch_failed("that is not a URL this browser can open");
		return;
	}

	start_fetch(&u, true);

}

static void load_image(uint32_t block, const html_line_t *l);

static void follow_link(uint32_t block, uint8_t idx) {

	static html_line_t l;
	url_t dest;

	if (page_fetch(&pg, block, 1, &l) != 1) return;
	if (idx >= l.nlinks) return;

	// An image box is clickable like a link, but clicking it loads
	// the picture in place rather than navigating away from the page
	// it illustrates.
	if (l.kind == HTML_IMAGE) { load_image(block, &l); return; }

	if (!url_resolve_str(&cur_url, l.links[idx], &dest)) {
		fetch_failed("that link is not a URL this browser can parse");
		return;
	}

	// A link to the same page with only a fragment is a jump, not a
	// fetch. Without this, clicking a table-of-contents entry
	// re-downloads the article.
	if (url_same_origin(&dest, &cur_url) &&
		!strcmp(dest.path, cur_url.path) &&
		dest.has_query == cur_url.has_query &&
		!strcmp(dest.query, cur_url.query)) {
		// No anchor index yet -- Phase 4. Go to the top, which is at
		// least honest about not knowing where the anchor is.
		scroll_to_top();
		return;
	}

	start_fetch(&dest, true);

}

static void go_back(void) {
	if (hist_at <= 0) return;
	hist_at--;
	{
		url_t u;
		if (url_parse(hist[hist_at], &u)) start_fetch(&u, false);
	}
}

static void go_forward(void) {
	if (hist_at + 1 >= hist_n) return;
	hist_at++;
	{
		url_t u;
		if (url_parse(hist[hist_at], &u)) start_fetch(&u, false);
	}
}

// -- the fetch service ---------------------------------------------

static void svc_start_next(void) {

	int i;

	if (svc_active >= 0 || state == W_RESOLVING || state == W_CONNECTING ||
		state == W_RECEIVING) return;

	for (i = 0; i < SVC_QUEUE; i++) {
		if (svc_q[i].used && svc_q[i].handle == 0) {
			url_t u;
			svc_active = i;
			if (!url_parse(svc_q[i].url, &u)) {
				fetch_failed("unparseable URL");
				return;
			}
			redirects = 0;
			fetch_is_head = svc_q[i].head;
			start_fetch(&u, false);
			return;
		}
	}

}

static void handle_web_fetch(const z_msg_t *msg) {

	z_obj_t *o;
	int slot = -1;

	for (int i = 0; i < SVC_QUEUE; i++)
		if (!svc_q[i].used) { slot = i; break; }

	if (slot < 0) {
		z_obj_t reply = z_obj_map(2);
		z_map_set(&reply, "ok", z_obj_uint32(0));
		z_map_set(&reply, "error", z_obj_str("web: fetch queue is full"));
		z_msg_new_send(msg->from, Z_WEB_FETCH_REPLY, msg->tag, reply);
		return;
	}

	if (msg->obj.type != Z_MAP ||
		!(o = z_map_find((z_obj_t *)&msg->obj, "url")) ||
		o->type != Z_STR || !o->val.str) {
		z_obj_t reply = z_obj_map(2);
		z_map_set(&reply, "ok", z_obj_uint32(0));
		z_map_set(&reply, "error", z_obj_str("web: fetch needs a url string"));
		z_msg_new_send(msg->from, Z_WEB_FETCH_REPLY, msg->tag, reply);
		return;
	}

	memset(&svc_q[slot], 0, sizeof(svc_q[slot]));
	svc_q[slot].used = true;
	svc_q[slot].pid = msg->from;
	svc_q[slot].tag = msg->tag;
	snprintf(svc_q[slot].url, URL_MAX, "%s", o->val.str);

	o = z_map_find((z_obj_t *)&msg->obj, "method");
	if (o && o->type == Z_STR && o->val.str &&
		(!strcmp(o->val.str, "HEAD") || !strcmp(o->val.str, "head")))
		svc_q[slot].head = true;

	svc_start_next();

}

static void handle_web_cancel(const z_msg_t *msg) {

	uint32_t h;

	if (msg->obj.type != Z_UINT32) return;
	h = msg->obj.val.uint32;

	for (int i = 0; i < SVC_QUEUE; i++) {
		if (svc_q[i].used && svc_q[i].handle == h &&
			svc_q[i].pid == msg->from) {
			if (svc_active == i) {
				sock_disconnect();
				svc_active = -1;
				state = W_IDLE;
			}
			svc_q[i].used = false;
		}
	}

	if (svc_streaming && svc_stream_handle == h) {
		zstream_producer_close(&svc_stream);
		svc_streaming = false;
	}

}

static void handle_stream_open(z_msg_t *msg) {

	z_obj_t *o;
	uint32_t h;
	int i, found = -1;

	if (svc_streaming) {
		zstream_reject(msg->from, msg->tag, "web: a body stream is already open");
		return;
	}

	if (msg->obj.type != Z_MAP ||
		!(o = z_map_find(&msg->obj, "handle")) || o->type != Z_UINT32) {
		zstream_reject(msg->from, msg->tag, "web: open needs {handle}");
		return;
	}

	h = o->val.uint32;

	for (i = 0; i < SVC_QUEUE; i++)
		if (svc_q[i].used && svc_q[i].handle == h && svc_q[i].pid == msg->from)
			found = i;

	// A stale handle is rejected rather than silently serving
	// whatever happens to be in the spool -- which by then is a
	// different page.
	if (found < 0 || spool_rd < 0) {
		zstream_reject(msg->from, msg->tag, "web: unknown or expired handle");
		return;
	}

	zstream_accept(&svc_stream, msg->from, msg->tag);
	svc_streaming = true;
	svc_stream_off = 0;
	svc_stream_handle = h;

}

// One chunk per call, driven from the idle path so that serving a
// large body does not stop this process reading messages.
static void svc_stream_pump(void) {

	static char buf[ZSTREAM_CHUNK_SIZE_DEFAULT];
	uint32_t n;

	if (!svc_streaming) return;

	if (svc_stream_off >= spool_len) {
		zstream_send_eof(&svc_stream);
		svc_streaming = false;
		return;
	}

	n = spool_read(NULL, svc_stream_off, buf, sizeof(buf));

	if (n == 0) {
		zstream_send_error(&svc_stream, "web: spool read failed");
		svc_streaming = false;
		return;
	}

	zstream_send_chunk(&svc_stream, buf, n);
	svc_stream_off += n;

}

// -- input ---------------------------------------------------------


static void select_link(int dir) {

	if (nhits == 0) return;

	if (sel_link < 0) sel_link = (dir > 0) ? 0 : nhits - 1;
	else sel_link += dir;

	// Off either end scrolls rather than wrapping: wrapping to the
	// other end of the screen is disorienting, and the next link is
	// almost always just off the edge.
	if (sel_link >= nhits) {
		sel_link = -1;
		scroll_by(lines_per_screen() / 2);
		if (nhits) sel_link = 0;
	} else if (sel_link < 0) {
		sel_link = -1;
		scroll_by(-(lines_per_screen() / 2));
		if (nhits) sel_link = nhits - 1;
	}

	repaint();

}

static void handle_key(uint32_t k, uint8_t mods) {

	(void)mods;

	// While the URL field has focus, typing goes there and nowhere
	// else -- otherwise 'g' in the middle of a hostname would open a
	// dialog and space would scroll the page out from under it.
	if (url_focus) {

		switch (k) {

		case '\r':
		case '\n':
			url_focus = false;
			url_editing = false;
			if (url_buf[0]) go_to_text(url_buf);
			else draw_bar();
			return;

		case 0x1b:				// Escape: put back what was there
			url_focus = false;
			url_editing = false;
			draw_bar();
			return;

		default:
			if (z_edit_key(&url_edit, k)) {
				url_editing = true;
				draw_bar();
			}
			return;

		}

	}

	switch (k) {

	case Z_KEY_DOWN:      scroll_by(1); break;
	case Z_KEY_UP:        scroll_by(-1); break;
	case Z_KEY_PAGEDOWN:
	case ' ':             scroll_by(lines_per_screen() - 2); break;
	case Z_KEY_PAGEUP:    scroll_by(-(lines_per_screen() - 2)); break;
	case Z_KEY_HOME:      scroll_to_top(); break;
	case Z_KEY_END:       scroll_to_end(); break;

	case '\t':            select_link(1); break;

	case '\r':
	case '\n':
		if (sel_link >= 0 && sel_link < nhits)
			follow_link(hits[sel_link].block, hits[sel_link].link);
		break;

	case 'g':
	case 0x0C:				// Ctrl+L, as everywhere else
		// Focus the field rather than opening a dialog. The URL is
		// already on screen and already the thing being edited; a
		// modal box over the top of it was a step that existed only
		// because there was nowhere to type.
		url_focus = true;
		url_editing = true;
		z_edit_set(&url_edit, cur_url_text);
		draw_bar();
		break;

	case 'b':
	case 0x08:				// Backspace
		go_back();
		break;

	case 'f':             go_forward(); break;

	case 'r':
		if (cur_url_text[0]) {
			url_t u;
			if (url_parse(cur_url_text, &u)) start_fetch(&u, false);
		}
		break;

	default:
		break;

	}

}

static void handle_mouse(uint32_t packed) {

	int cx, cy;
	uint8_t buttons = (uint8_t)Z_WM_UNPACK_MOUSE_BUTTONS(packed);

	if (!z_win_mouse_content_xy(&win, packed, &cx, &cy)) return;

	if (z_scrollbar_has_pointer(&sbar, cx, cy)) {
		if (z_scrollbar_mouse(&sbar, cx, cy, buttons)) {
			top.block = (uint32_t)sbar.value;
			top.sub = 0;
			if (top.block >= page_blocks(&pg) && page_blocks(&pg))
				top.block = page_blocks(&pg) - 1;
			sel_link = -1;
			repaint();
		}
		return;
	}

	// The toolbar. Handled before the W_READY test below, so the
	// buttons and the field still work while a page is loading --
	// which is exactly when someone wants Back or a different URL.
	{
		toolbar_t tb;
		toolbar_hit_t hit;

		toolbar_geom(z_win_content_w(&win), &tb);
		hit = toolbar_hit(&tb, cx, cy);

		if (hit != TB_HIT_NONE) {

			if (!(buttons & 1)) return;

			switch (hit) {

			case TB_HIT_BACK:
				if (hist_at > 0) go_back();
				break;

			case TB_HIT_FORWARD:
				if (hist_at + 1 < hist_n) go_forward();
				break;

			case TB_HIT_FIELD:
				if (!url_focus) {
					url_focus = true;
					url_editing = true;
					z_edit_set(&url_edit, cur_url_text);
				}
				z_edit_click(&url_edit, cx, tb.field_x, &z_font_5x8);
				draw_bar();
				break;

			default:
				break;

			}

			return;

		}
	}

	// A click in the page gives focus back, so typing scrolls again.
	if (url_focus && (buttons & 1)) {
		url_focus = false;
		url_editing = false;
		draw_bar();
	}

	if (!(buttons & 1) || state != W_READY) return;

	// Hit rectangles are in SCREEN coordinates, because that is what
	// layout.c draws in; the pointer arrives content-relative.
	{
		int sx = crect.x0 + cx;
		int sy = crect.y0 + cy;

		for (int i = 0; i < nhits; i++) {
			const layout_hit_t *h = &hits[i];
			if (sx >= h->x && sx < h->x + h->w &&
				sy >= h->y && sy < h->y + h->h) {
				follow_link(h->block, h->link);
				return;
			}
		}
	}

}

// -- messages ------------------------------------------------------

static void handle_port_msg(z_msg_t *msg) {

	switch (msg->subject) {

	case Z_PORT_CONNECTED:
		if (state != W_CONNECTING) {
			// A CONNECTED that arrives when we are not waiting for
			// one belongs to a connection already abandoned. Worth
			// saying: it means the teardown and the new attempt have
			// overlapped.
			printf("web: stray Z_PORT_CONNECTED in state %d\n", (int)state);
			break;
		}
		port_msgs = 0;
		port_bytes = 0;
		sock.peer_pid = msg->from;
		sock.conn_id = (msg->obj.type == Z_UINT32) ? msg->obj.val.uint32 : 0;
		sock.connected = true;
		sock_open = true;
		tm.t_connect = web_ms() - tm.t_fetch - tm.t_dns;
#if WEB_TLS
		if (tls_in_use()) {
			// The request cannot go out until the handshake finishes.
			// It is sent from the Z_PORT_DATA path instead, the first
			// time tls_established() becomes true.
			tls_init(&tls, fetch_url.host, tls_on_send, tls_on_data, NULL);

			// The clock and the trust store. Both are refusals when
			// missing, not things to work around.
			{
				uint32_t sec = 0, sub = 0;
				if (!z_rtc_valid()) {
					fetch_failed("the clock is not set, so certificate "
						"dates cannot be checked -- run net for NTP");
					return;
				}
				z_rtc_get(&sec, &sub);
				(void)sub;
				tls_set_verify(&tls, (int64_t)sec, web_find_root, NULL);
			}

			tls_req_sent = false;
			snprintf(status, sizeof(status), "TLS handshake with %s...",
				fetch_url.host);
			repaint();
			tls_send_failed = false;
			tls_rx_bytes = 0;
			tls_start_ticks = z_uptime_ticks();
			tls_t0 = z_uptime_ticks();
			if (!tls_start(&tls)) { fetch_failed(tls_error(&tls)); return; }
			if (tls_send_failed) {
				fetch_failed("could not send the TLS handshake to net");
				return;
			}
			printf("web: tls: ClientHello sent to %s\n", fetch_url.host);
			state = W_RECEIVING;
			break;
		}
#endif
		send_request();
		break;

	case Z_PORT_REFUSED:
		fetch_failed((msg->obj.type == Z_STR && msg->obj.val.str)
			? msg->obj.val.str : "the connection was refused");
		break;

	case Z_PORT_DATA: {

		uint32_t len = z_blob_len(&msg->obj);
		void *data = z_blob_data(&msg->obj);

		// COPY, then ack, then process.
		//
		// Acking early matters: it frees net's pending-send slot, and
		// with a window of Z_PORT_MAX_PENDING_SENDS, holding the ack
		// until parsing finishes leaves net with bytes TCP has
		// already acknowledged and nowhere to put them.
		//
		// But the ack is a SYSCALL, and `data` points into the
		// message payload. Once net is told the slot is free it can
		// run and reuse that memory -- while this side is still
		// reading it. That is not theoretical: it corrupted a
		// 4KB certificate record on the wire, decrypting the first
		// record of a flight correctly and failing the second, with
		// byte counts on both sides agreeing exactly because nothing
		// was lost, only overwritten.
		//
		// Copying first costs one memcpy per chunk and makes the
		// lifetime question go away.
		if (len > sizeof(port_rx)) {
			printf("web: oversized port payload %lu\n", (unsigned long)len);
			z_port_send_ack(msg);
			break;
		}
		if (data && len) memcpy(port_rx, data, len);

		z_port_send_ack(msg);

		// Data from a connection that is no longer the current one is
		// ACKED AND DISCARDED.
		//
		// Closing a socket and immediately opening another -- which
		// is what following a redirect does -- leaves net still
		// flushing its queue from the old session while this app is
		// blocked in DNS for the new one. Those bytes arrive after
		// the mailbox drain and before the new connection's first
		// record, and feeding them to a fresh TLS context produced
		// "oversized record": a record length read out of the middle
		// of the previous page's certificate.
		//
		// The ack still has to happen, or net's pending slot never
		// frees.
		if (!sock_open || !sock.connected ||
			msg->tag != sock.conn_id) {
			printf("web: dropped %lu stale bytes (conn %lu, want %lu)\n",
				(unsigned long)z_blob_len(&msg->obj),
				(unsigned long)msg->tag, (unsigned long)sock.conn_id);
			break;
		}

		// Arrival accounting.
		//
		// "net relayed 4524 bytes and web saw 0" is the shape of the
		// failure being chased, and it cannot be told apart from
		// "web never ran" without counting on this side too.
		// Counted always, printed only at the end of a connection.
		// The serial console is visibly interleaving output between
		// processes, and a line per chunk makes the lines that matter
		// unreadable.
		port_msgs++;
		port_bytes += len;

		if (data && len && state == W_RECEIVING)
			transport_recv(port_rx, len);

#if WEB_TLS
		if (tls_in_use() && tls_failed(&tls)) {
			fetch_failed(tls_error(&tls));
			break;
		}
		if (tls_in_use() && !tls_req_sent && tls_established(&tls)) {
			tls_req_sent = true;
			tm.t_ready = web_ms() - tm.t_fetch - tm.t_dns - tm.t_connect;
			printf("web: tls: up after %lu bytes, %lu ms\n",
				(unsigned long)tls_rx_bytes, (unsigned long)tm.t_ready);
			send_request();
		}
		// The server's close_notify ends the body, exactly as a TCP
		// close does for plain HTTP.
		if (tls_in_use() && tls_closed(&tls) && state == W_RECEIVING) {
			http_eof(&http);
			if (http_error(&http)) fetch_failed(http_error(&http));
			else response_done();
			break;
		}
#endif

		if (state == W_RECEIVING && http_done(&http)) response_done();
		else if (state == W_RECEIVING && http_error(&http))
			fetch_failed(http_error(&http));
		break;
	}

	case Z_PORT_CLOSE:
		sock.connected = false;
		sock_open = false;
		// A reused connection the server had already closed. Retry
		// once on a fresh socket -- see fetch_reused.
		if (fetch_reused && state == W_RECEIVING && port_bytes == 0) {
			printf("web: reused connection was closed, retrying fresh\n");
			conn_forget();
			sock_open = false;
			sock.connected = false;
			fetch_retrying = true;
			spool_close();
			start_fetch(&fetch_url, false);
			break;
		}

#if WEB_TLS
		// A TLS connection that closes before the handshake finishes
		// is a TLS failure, and reporting it through http_eof() gave
		// "connection closed before any response" -- which is true of
		// the HTTP layer and says nothing about what actually went
		// wrong one layer down.
		if (tls_in_use() && !tls_established(&tls) && state == W_RECEIVING) {
			printf("web: tls: closed during handshake after %lu bytes "
				"from the server\n", (unsigned long)tls_rx_bytes);
			fetch_failed(tls_error(&tls)
				? tls_error(&tls)
				: "the server closed the connection during the TLS "
				  "handshake");
			break;
		}
#endif
		if (state == W_RECEIVING) {
			http_eof(&http);
			if (http_error(&http)) fetch_failed(http_error(&http));
			else response_done();
		}
		break;

	default:
		break;

	}

}

int main(void) {

	printf("web: starting\n");

	if (z_win_create_flags(&win, "web", WIN_W, WIN_H, -1, -1,
		Z_WIN_FLAG_CLOSE_ICON | Z_WIN_FLAG_CLOSE_KILLS_OWNER |
		Z_WIN_FLAG_RESIZABLE | Z_WIN_FLAG_MIN_IS_CREATE) != Z_OK) {
		printf("web: failed to create window -- is wm running?\n");
		return 1;
	}

	// Registered immediately, before any network work, for the same
	// reason net.c registers first: this process can receive messages
	// from the moment it starts, which is when it should be findable.
	// A lookup then either succeeds or the process genuinely is not
	// running.
	if (z_pid_register("web", self_name, sizeof(self_name)))
		printf("web: registered as '%s'\n", self_name);
	else
		printf("web: name registration FAILED -- the fetch service "
			"will not be findable\n");

	z_scrollbar_init(&sbar, &win, Z_SB_VERT);

	// Pick the spool location once, by trying it.
	//
	// There is no "is there a ramdisk" call in the app-facing file
	// API, and adding one to answer a question a create-and-close
	// already answers would be a worse trade than the one write.
	{
		int probe = fs_open_write(SPOOL_PATH_RAM);
		if (probe >= 0) {
			fs_close_handle(probe);
			spool_path = SPOOL_PATH_RAM;
		}
		printf("web: spool: %s\n", spool_path);
	}

	// The URL bar starts focused, holding "https://".
	//
	// There is nothing else to do in an empty browser, and the caret
	// sits after the scheme so a hostname can simply be typed. https
	// rather than http because a bare hostname on port 80 is a
	// redirect at best on most of the web now, and this browser pays
	// for a whole extra connection to follow one.
	z_edit_init(&url_edit, url_buf, sizeof(url_buf), "https://");
	url_focus = true;
	url_editing = true;

	page_init(&pg, spool_read, NULL);

#if WEB_TLS
	verify_set_clock(web_ms);
#endif

	snprintf(status, sizeof(status), "web -- type a URL and press Enter");
	relayout();
	repaint();

	{
		char arg[URL_MAX];
		if (z_launch_arg_take(arg, sizeof(arg))) {
			// Launched with a URL: that is where the reader wants to
			// be, so the bar gives up focus and the page gets it.
			url_focus = false;
			url_editing = false;
			go_to_text(arg);
		}
	}

	for (;;) {

		z_msg_t msg;

		while (z_msg_read(&msg) == Z_OK) {

			switch (msg.subject) {

			case Z_WM_KEY:
				if (msg.obj.type != Z_UINT32) break;
				if (!Z_WM_UNPACK_KEY_PRESSED(msg.obj.val.uint32)) break;
				handle_key(Z_WM_UNPACK_KEY_KEYSYM(msg.obj.val.uint32),
					(uint8_t)Z_WM_UNPACK_KEY_MODIFIERS(msg.obj.val.uint32));
				break;

			case Z_WM_MOUSE:
				if (msg.obj.type == Z_UINT32) handle_mouse(msg.obj.val.uint32);
				break;

			case Z_WM_REDRAW:
				z_win_apply_redraw(&win, msg.obj.type == Z_UINT32
					? msg.obj.val.uint32 : 0);
				repaint_for_wm();
				break;

			case Z_WM_WINDOW_RESIZED:
				if (z_win_apply_resized(&win, &msg.obj)) {
					// The viewport keeps its top BLOCK across a
					// resize but not its sub-line: the block it was
					// showing now wraps differently, and a sub-line
					// index into the old wrapping means nothing.
					top.sub = 0;
					sel_link = -1;
					repaint();
				}
				break;

			case Z_PORT_CONNECTED:
			case Z_PORT_REFUSED:
			case Z_PORT_DATA:
			case Z_PORT_CLOSE:
				handle_port_msg(&msg);
				break;

			case Z_PORT_DATA_ACK:
				z_port_handle_ack(&sock, &msg);
				break;

			case Z_WEB_FETCH:
				handle_web_fetch(&msg);
				break;

			case Z_WEB_CANCEL:
				handle_web_cancel(&msg);
				break;

			case Z_STREAM_OPEN:
				handle_stream_open(&msg);
				break;

			case Z_STREAM_PULL:
				svc_stream_pump();
				break;

			case Z_STREAM_ABORT:
				if (svc_streaming) {
					zstream_producer_close(&svc_stream);
					svc_streaming = false;
				}
				break;

			default:
				break;

			}

		}

		// -- idle work --
		//
		// Indexing and stream serving both happen here, a bounded
		// amount at a time, so that neither stops this process from
		// reading messages. A 350KB page indexed in one go is
		// hundreds of milliseconds of no repaints, which wm reports
		// as a missed redraw ack.
#if WEB_TLS
		// A handshake that has stalled. See tls_start_ticks.
		if (tls_in_use() && state == W_RECEIVING && !tls_established(&tls) &&
			sock_open &&
			z_uptime_ticks() - tls_start_ticks > TLS_HANDSHAKE_TIMEOUT_TICKS) {
			char msg[128];
			uint32_t recs = 0, msgs = 0;
			uint8_t last = 0, tstate = 0;
			tls_progress(&tls, &recs, &msgs, &last, &tstate);
			snprintf(msg, sizeof(msg),
				"TLS handshake stalled -- %lu bytes, %lu records, "
				"%lu messages (last type %u), state %u",
				(unsigned long)tls_rx_bytes, (unsigned long)recs,
				(unsigned long)msgs, (unsigned)last, (unsigned)tstate);
			printf("web: tls: %s\n", msg);
			fetch_failed(msg);
			continue;
		}
#endif

		if (state == W_INDEXING) {

			{
				uint32_t t0 = web_ms();
				page_index_more(&pg, INDEX_BUDGET);
				tm.index_ms += web_ms() - t0;
			}

			if (page_indexed(&pg)) {

				uint32_t total = web_ms() - tm.t_fetch;
				uint32_t body_ms = tm.t_body ? tm.t_body : 1;

				// One line per phase. The console interleaves between
				// processes, so short lines survive where a long one
				// arrives unreadable.
				printf("web: time: dns %lu, connect %lu, secure %lu ms\n",
					(unsigned long)tm.t_dns, (unsigned long)tm.t_connect,
					(unsigned long)tm.t_ready);
				printf("web: time: body %lu B in %lu ms = %lu B/s\n",
					(unsigned long)tm.body_bytes, (unsigned long)body_ms,
					(unsigned long)(tm.body_bytes * 1000u / body_ms));
				printf("web: time: of that, crypto %lu ms, card %lu ms\n",
					(unsigned long)tm.crypto_ms, (unsigned long)tm.spool_ms);
				// Reading versus parsing. With the spool in RAM the
				// index barely got faster, which says the time is in
				// html.c rather than in storage -- but that was an
				// inference, and this makes it a measurement.
				printf("web: time: index %lu ms (read %lu, parse %lu) "
					"for %lu blocks\n",
					(unsigned long)tm.index_ms, (unsigned long)tm.read_ms,
					(unsigned long)(tm.index_ms > tm.read_ms
						? tm.index_ms - tm.read_ms : 0),
					(unsigned long)page_blocks(&pg));
				printf("web: time: total %lu ms\n", (unsigned long)total);

				if (conn_reusable && sock_open)
					conn_idle_deadline = web_ms() * 0 + z_uptime_ticks()
						+ CONN_IDLE_TICKS;

				state = W_READY;
				if (page_title(&pg)[0]) z_win_set_title(&win, page_title(&pg));
				repaint();
			} else {
				// Cheap progress, without a full repaint per pass.
				snprintf(status, sizeof(status), "reading... %lu blocks",
					(unsigned long)page_blocks(&pg));
				draw_bar();
			}

			continue;		// stay hot until the index is finished

		}

		if (svc_streaming) { svc_stream_pump(); continue; }

		// Give net's only TCB back once nothing has used the open
		// connection for a while.
		if (conn_idle_deadline && state != W_RECEIVING &&
			(int32_t)(z_uptime_ticks() - conn_idle_deadline) > 0) {
			printf("web: closing idle connection to %s\n", conn_host);
			sock_disconnect();
			conn_forget();
		}

		svc_start_next();

#if WEB_TLS
		// Wake periodically during a handshake so the stall check
		// above actually runs -- z_proc_wait(0) blocks until a
		// message, and a stalled handshake is precisely the case
		// where no message is coming.
		if (tls_in_use() && state == W_RECEIVING && !tls_established(&tls)) {
			z_proc_wait(732);
			continue;
		}
#endif

		z_proc_wait(0);

	}

	return 0;

}
