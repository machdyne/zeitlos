/*
 * Zeitlos
 *
 * Screen streaming over the ESP32 link. The 640x480 1bpp framebuffer
 * is treated as 30 stripes of 16 rows (1280 bytes each -- one UDP
 * datagram, one link frame). A scan hashes all thirty against what was
 * last sent and ships the ones that changed; a full pass is forced
 * every so often so a stripe lost in flight (this is UDP on purpose)
 * heals itself. The consumer keeps the shadow.
 *
 * Scans happen only while a browser is connected, and at a capped
 * rate. See the comment on SCAN_TICKS below for why both.
 *
 * ONE FRAME IS ONE INSTANT
 *
 * This used to read the framebuffer stripe by stripe, all the way
 * through: hash 16 rows, compress them, clock them out of the UART,
 * then move to the next 16 rows. A scan takes tens of milliseconds and
 * the screen does not hold still for it, so the thirty stripes were
 * thirty different moments. With a window being dragged, the browser
 * assembled a frame holding the elastic band in several places at once
 * -- the "trocitos de los bordes" seen over the remote desktop and
 * never locally, where the XOR band is drawn and undone in pairs.
 * Measured before this change: 91% of the frames a drag produced held
 * the band at up to five different x positions (tools/frame_mosaic.py).
 *
 * The cure is a snapshot. One tight pass copies the whole framebuffer
 * into this process's own memory AND hashes it on the way through, and
 * everything after that -- comparison, PackBits, transmission -- reads
 * the copy. Whatever the wm does next cannot reach a scan already in
 * flight. There is no double buffer in the SOC to read from instead;
 * this is 38400 bytes of net's own region standing in for one.
 *
 * It is close to free because it replaces reads rather than adding
 * them: the hash pass already read all 9600 words out of VRAM, and
 * every stripe that got sent was then read a SECOND time into a
 * staging buffer for PackBits. Fusing copy and hash trades those
 * second reads for the writes of the copy.
 *
 * A snapshot is only half of it. The browser paints each stripe as it
 * lands, so a consistent frame still arrives in pieces. So each stripe
 * also carries a frame trailer (see stripe_send) saying which frame it
 * belongs to and whether it is the last of it, and the viewer page
 * assembles off-screen and shows whole frames.
 */

#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#include "../../common/zeitlos.h"	/* z_uptime_ticks, TICKS_PER_SEC */
#include "screen.h"
#include "udp.h"
#include "esp32link.h"
#include "packbits.h"

#define FB_BASE        0x20000000
#define STRIPES        30
#define STRIPE_WORDS   320	/* 640/8 * 16 rows / 4 */
#define SCREEN_PORT    7777
#define SRC_PORT       7778
#define FORCE_TICKS    (5 * TICKS_PER_SEC)	/* full resend interval (heals a
					   lost stripe); rare, so it does not
					   flood net's loop */
#define TICKS_PER_SEC  732

/* WHO IS WATCHING, AND HOW OFTEN
 *
 * This used to hash 8 of the 30 stripes on every call, and net called
 * it once per kernel tick: a complete pass over the 640x480 framebuffer
 * roughly 180 times a second, ~1.3 MB/s of reads across the same
 * arbiter the GPU and the video scanout use -- and it ran whether or
 * not a browser was connected, because the only condition was having a
 * gateway. What that costs the foreground was measured: 80-120 us
 * per character cell of a terminal redraw went to net.
 *
 * Two limits now:
 *
 * (1) A viewer has to be connected. See screen_set_viewer(): net
 *     cannot see the WebSocket, which terminates on the ESP32, so
 *     esp32link.c infers it and tells us.
 *
 * (2) A scan is COMPLETE and PACED. Complete -- every dirty stripe in
 *     one pass -- because the old budget spread split one frame across
 *     ~4 calls with the picture moving between them, which is exactly
 *     the mid-drag "comb" of leftover edges seen over the remote link
 *     (the cooldown path already did whole scans for that reason; now
 *     there is only the one path). Paced because 180 fps was never
 *     useful: the browser draws what it gets and the wire is 3 Mbaud.
 *     SCAN_TICKS is the ceiling with a quiet screen, BUSY_SCAN_TICKS
 *     the one under heavy churn (a drag, gpu3d animating), where each
 *     pass ships many stripes and every one of them is bytes clocked
 *     out by hand in uart1_putc().
 */
#define BUSY_STRIPES   10
#define SCAN_TICKS     (TICKS_PER_SEC / 20)	/* 20 fps ceiling */
#define BUSY_SCAN_TICKS (TICKS_PER_SEC / 15)	/* 15 fps under churn */

static uint32_t sent_hash[STRIPES];
static uint32_t last_force_tick;
static uint16_t seq;
static uint16_t frame_seq;
static uint32_t last_scan_tick;
static uint32_t scan_gap = SCAN_TICKS;
static int      viewer;

/* The frame this scan is about to send, and the only thing here that
 * costs real memory: 38400 bytes of net's ~300 KB region. */
static uint32_t snap[STRIPES * STRIPE_WORDS];

#ifdef SCREEN_PROFILE
#include <stdio.h>
static uint32_t rdcycle(void)
{
	uint32_t v;
	__asm__ volatile ("rdcycle %0" : "=r"(v));
	return v;
}
#endif

/* THE HASH HAS TO SEE A VERTICAL LINE
 *
 * This was `h ^= w[i]; h = rotl(h, 5);` and it could not tell a stripe
 * with a full-height vertical line in it from a blank one. Not a
 * near miss -- the same hash, exactly, for every column.
 *
 * Why: xor and rotate are both linear, so the hash is the xor of every
 * word rotated by its distance from the end, and two words land on the
 * same rotation whenever their indices differ by a multiple of 32. A
 * row is 20 words, so rows r and r+8 alias (20*8 = 160, a multiple of
 * 32), and a 16-row stripe is exactly eight such pairs. A vertical
 * line writes the SAME bit in the SAME word of every row, so each pair
 * cancels and the whole line contributes nothing. Any rotate-and-xor
 * has this hole: with an odd rotate the aliasing distance works out to
 * 8 rows whatever the amount, and an even one is worse.
 *
 * What that cost, on screen: dragging a window over the remote desktop
 * left "trocitos de los bordes" behind -- the elastic band's two
 * VERTICAL edges, the only part of the picture made of full-height
 * lines, were the part net could not see change. Only the stripes
 * holding the band's horizontal top and bottom were ever resent, and
 * the leftovers survived until the five-second full resend wiped them.
 * Locally the same drag is spotless, which is what kept the blame on
 * compositing for so long: the wm was right, the change detector was
 * blind. Measured: a drag shipped 2-4 stripes a frame where 18 had
 * changed.
 *
 * `h += w[i]` afterwards is the whole fix. Addition carries across bit
 * positions, so the hash stops being linear over xor and the pairs no
 * longer cancel: 0 of 536 one-pixel moves of a vertical line missed,
 * against 536 of 536 before, and single-bit flips were already caught
 * by both. One instruction per word, and no multiply -- rv32im has
 * one, but a 32-step sequential multiplier on a board without the DSP
 * option would cost more than the copy this loop exists for.
 *
 * Copy and hash are fused deliberately: a second pass to hash the copy
 * would cost as much as the reads it saves, and the point of the
 * exercise is that the snapshot is not paid for twice.
 *
 * Returns how many stripes differ from what was last sent, and fills
 * dirty[] with their indices in ascending order -- the consumer needs
 * the count before the first stripe goes out, to mark the last one. */
static int snapshot(int *dirty)
{
	const volatile uint32_t *v = (const volatile uint32_t *)FB_BASE;
	uint32_t *d = snap;
	int n = 0;
#ifdef SCREEN_PROFILE
	/* rdcycle is wall clock and this loop is longer than a timeslice,
	 * so a single reading can be mostly somebody else's work; the
	 * MINIMUM over many scans is the one that means anything. */
	static uint32_t prof_min = 0xffffffffu, prof_sum, prof_n;
	uint32_t t0 = rdcycle();
#endif

	for (int idx = 0; idx < STRIPES; idx++) {
		uint32_t h = 0x9e3779b9u ^ (uint32_t)idx;
		/* by fours: the loop arithmetic was a fifth of the work
		 * in a body this small */
		for (int i = 0; i < STRIPE_WORDS; i += 4) {
			uint32_t a = v[0], b = v[1], c = v[2], e = v[3];
			d[0] = a; d[1] = b; d[2] = c; d[3] = e;
			h ^= a; h = ((h << 5) | (h >> 27)) + a;
			h ^= b; h = ((h << 5) | (h >> 27)) + b;
			h ^= c; h = ((h << 5) | (h >> 27)) + c;
			h ^= e; h = ((h << 5) | (h >> 27)) + e;
			v += 4; d += 4;
		}
		if (!h)
			h = 1;		/* 0 means "never sent" */
		if (h != sent_hash[idx]) {
			sent_hash[idx] = h;
			dirty[n++] = idx;
		}
	}
#ifdef SCREEN_PROFILE
	uint32_t dt = rdcycle() - t0;
	if (dt < prof_min)
		prof_min = dt;
	prof_sum += dt;
	if (++prof_n == 128) {
		printf("screen: snap %lu cyc min over %lu scans, mean %lu\n",
			(unsigned long)prof_min, (unsigned long)prof_n,
			(unsigned long)(prof_sum / prof_n));
		prof_sum = 0;
		prof_n = 0;
	}
#endif
	return n;
}

/* THE FRAME TRAILER
 *
 * Four bytes after the PackBits payload: a magic byte, the frame this
 * stripe belongs to, and whether it closes that frame.
 *
 *	0x5A | fseq_lo | fseq_hi | flags   (bit 0 = last of frame)
 *
 * Behind the payload rather than in the 6-byte header because the
 * header does not survive the trip: the ESP32 relays [idx, len, data]
 * to the browser and drops everything else (screend.c's udp_task), so
 * anything the page must see has to travel inside `data`. Trailing
 * bytes are the one place where that is invisible to a decoder that
 * does not know about them -- PackBits stops the moment it has
 * produced its 1280 bytes -- so an ESP32 running the old firmware and
 * a browser running the old page both keep working, byte for byte,
 * with only the frame assembly missing. tools/grab_fb.py likewise.
 *
 * A reader tells trailer from payload by arithmetic, not by the magic
 * alone: the trailer is there only if there are exactly four bytes
 * left over after the decode consumed what it needed.
 */
#define TRAILER_MAGIC  0x5A
#define TRAILER_LAST   0x01

static void stripe_send(uint32_t gw_ip, int idx, int last)
{
	/* 6-byte header + PackBits payload (worst case a shade over raw)
	 * + the 4-byte frame trailer */
	static uint8_t pkt[6 + STRIPE_WORDS * 4 + 64];
	const uint8_t *raw = (const uint8_t *)(snap + idx * STRIPE_WORDS);
	int clen = packbits(raw, STRIPE_WORDS * 4, pkt + 6);
	pkt[0] = 'Z';
	pkt[1] = 'S';
	pkt[2] = (uint8_t)idx;
	pkt[3] = 1;			/* 1 = PackBits */
	pkt[4] = (uint8_t)(seq & 0xff);
	pkt[5] = (uint8_t)(seq >> 8);
	seq++;
	pkt[6 + clen + 0] = TRAILER_MAGIC;
	pkt[6 + clen + 1] = (uint8_t)(frame_seq & 0xff);
	pkt[6 + clen + 2] = (uint8_t)(frame_seq >> 8);
	pkt[6 + clen + 3] = last ? TRAILER_LAST : 0;
	udp_send(gw_ip, SCREEN_PORT, SRC_PORT, pkt, 6 + clen + 4);
}

void screen_reset(void)
{
	memset(sent_hash, 0, sizeof(sent_hash));
}

/* Told by esp32link.c, which is the only place that can know: the
 * WebSocket ends on the ESP32. See its viewer_present(). */
void screen_set_viewer(int present)
{
	viewer = present;
}

/* Ticks until the next scan is due, for net's main loop to sleep on;
 * 0 when there is nothing to stream and no deadline to keep. */
uint32_t screen_idle_ticks(void)
{
	if (!viewer)
		return 0;
	uint32_t gone = z_uptime_ticks() - last_scan_tick;
	return gone >= scan_gap ? 1 : scan_gap - gone;
}

void screen_poll(uint32_t gw_ip)
{
	if (!gw_ip || !viewer)
		return;

	uint32_t now = z_uptime_ticks();
	if ((now - last_scan_tick) < scan_gap)
		return;
	last_scan_tick = now;

	int dirty[STRIPES];
	int sends = snapshot(dirty);
	frame_seq++;
	int saw_input = 0;
	for (int k = 0; k < sends; k++) {
		/* Mouse/keyboard land in the BRAM FIFO unsolicited. Drain
		 * them BETWEEN stripes so a 20-stripe scan (300-700 ms of
		 * uart1_putc) does not leave the pointer sitting on a
		 * position the browser sent half a second ago. Do not
		 * yield: a pause mid-ZNIC-frame trips the ESP32's UART
		 * idle timeout and the stripe CRC-fails. */
		if (esp32link_pump_input())
			saw_input = 1;
		/* A drag with pending motion: finish THIS snapshot
		 * (cutting it paints the band in two places)
		 * but skip the rest of a full-frame force-resend. Those
		 * stripes were only dirty because we zeroed the hashes,
		 * and holding the UART for 30 of them is the stall the
		 * visor feels as "seconds". */
		if (saw_input && esp32link_vmouse_buttons() &&
				sends >= STRIPES && k > 0 &&
				k < sends - 1) {
			stripe_send(gw_ip, dirty[k], 1);
			for (int j = k + 1; j < sends; j++)
				sent_hash[dirty[j]] = 0;
			sends = k + 1;
			break;
		}
		stripe_send(gw_ip, dirty[k], k == sends - 1);
	}
	/* Next snapshot as soon as wm has painted the new position. */
	if (saw_input && esp32link_vmouse_buttons())
		scan_gap = 1;
	else
		scan_gap = (sends > BUSY_STRIPES) ? BUSY_SCAN_TICKS : SCAN_TICKS;

	if (now - last_force_tick >= FORCE_TICKS) {
		if (esp32link_vmouse_buttons() || sends > BUSY_STRIPES) {
			/* postpone the heal-the-viewer pass until the
			 * pointer is up and the scan is small again */
		} else {
			last_force_tick = now;
			memset(sent_hash, 0, sizeof(sent_hash));
		}
	}
}
