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
 * The copy, the hash, the DIRTY register and the frame trailer live
 * in zscreen (sw/common/zscreen.c), which does not know this is UDP.
 * What stays here is the ZS header, the shadow of stripes a viewer is
 * still owed, the pacing, and pumping the visor's pointer between
 * stripes. Nothing on the wire changes.
 */

#include <stdint.h>
#include <stdio.h>

#include "../../common/zeitlos.h"	/* z_uptime_ticks, TICKS_PER_SEC */
#include "../../common/zscreen.h"
#include "screen.h"
#include "udp.h"
#include "esp32link.h"

#define STRIPES        ZSCREEN_STRIPES
#define ALL_STRIPES    ZSCREEN_ALL
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

/* Stripes the viewer must be sent whatever their hash says: all of
 * them for a new viewer or the periodic resend, and any a drag cut
 * out of a full resend. */
static uint32_t owed = ALL_STRIPES;
/* The next scan reads every stripe: FULL_RESET after screen_reset(),
 * when the hashes mean nothing; FULL_VERIFY for the periodic resend,
 * when they do and a changed stripe with no DIRTY bit is a miss. */
#define FULL_NONE      ZSCREEN_FULL_NONE
#define FULL_RESET     ZSCREEN_FULL_RESET
#define FULL_VERIFY    ZSCREEN_FULL_VERIFY
static int      full_next = FULL_RESET;
static uint32_t last_force_tick;
static uint16_t seq;
static uint16_t frame_seq;
static uint32_t last_scan_tick;
static uint32_t scan_gap = SCAN_TICKS;
static int      viewer;

/* The frame this scan is about to send: 38400 bytes, inside zscreen,
 * of net's ~300 KB region. Left zeroed so it stays in .bss; dirty_hw
 * 0 means "not probed yet". */
static zscreen_t scr;

static void stripe_send(uint32_t gw_ip, int idx, int last)
{
	/* 6-byte header + PackBits payload + the 4-byte frame trailer.
	 * The payload buffer has to hold the PackBits worst case; the
	 * literal-run fallback, when it is used, overwrites that. */
	static uint8_t pkt[6 + ZSCREEN_PACK_WORST + ZSCREEN_TRAILER];
	const uint8_t *raw =
		(const uint8_t *)(scr.snap + idx * ZSCREEN_STRIPE_WORDS);
	int n = zscreen_encode(raw, pkt + 6, frame_seq, last);
	pkt[0] = 'Z';
	pkt[1] = 'S';
	pkt[2] = (uint8_t)idx;
	pkt[3] = 1;			/* 1 = PackBits */
	pkt[4] = (uint8_t)(seq & 0xff);
	pkt[5] = (uint8_t)(seq >> 8);
	seq++;
	udp_send(gw_ip, SCREEN_PORT, SRC_PORT, pkt, 6 + n);
}

void screen_reset(void)
{
	zscreen_forget(&scr);
	owed = ALL_STRIPES;
	full_next = FULL_RESET;
}

/* Told by esp32link.c, which is the only place that can know: the
 * WebSocket ends on the ESP32. See its viewer_present(). */
void screen_set_viewer(int present)
{
	if (viewer && !present && scr.dirty_hw == 1)
		printf("screen: %lu full check(s), %lu stripe(s) changed "
			"with no DIRTY bit\n", (unsigned long)scr.verify_passes,
			(unsigned long)scr.verify_missed);
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

	/* Read DIRTY, clear it, then copy. zscreen owns that order. */
	int full = full_next;
	full_next = FULL_NONE;
	uint32_t missed = 0;
	uint32_t changed = zscreen_scan(&scr, full, &missed);
	if (missed)
		printf("screen: stripes %08lx changed with no DIRTY bit\n",
			(unsigned long)missed);

	uint32_t out = changed | owed;
	owed = 0;
	int dirty[STRIPES];
	int sends = 0;
	for (int idx = 0; idx < STRIPES; idx++)
		if (out & (1u << idx))
			dirty[sends++] = idx;
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
		 * stripes were only owed because of the resend, and
		 * holding the UART for 30 of them is the stall the visor
		 * feels as "seconds". What is cut stays owed. */
		if (saw_input && esp32link_vmouse_buttons() &&
				sends >= STRIPES && k > 0 &&
				k < sends - 1) {
			stripe_send(gw_ip, dirty[k], 1);
			for (int j = k + 1; j < sends; j++)
				owed |= 1u << dirty[j];
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
			owed = ALL_STRIPES;
			full_next = FULL_VERIFY;
		}
	}
}
