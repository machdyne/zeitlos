/*
 * Zeitlos -- UART PHY to the onboard ESP32 (ULX3S).
 *
 * Third NIC backend (NET_PHY=ESP32LINK). Speaks ZNIC over UART1;
 * 802.11 stays on the ESP32. See docs/esp32link.md and znic.h.
 *
 * Transport model (fw ver 2): every frame the ESP32 sends is the
 * reply to one frame we sent. RX_POLL is answered with the oldest
 * queued control message (HELLO, LINK, LOG), else a DATA frame, else
 * NOP; DATA is answered with DATA_ACK; STA with STA_ACK. Replies are
 * read from rtl/esp32_rxfifo.v, a 2 KiB block-RAM FIFO on the UART1
 * RX pin (the 16550's 16 bytes could not survive a time slice: at
 * 1 Mbaud, ~4 ms away from the CPU is 400 bytes). So nothing here
 * masks interrupts, and a reply that comes late is never thrown away:
 * whatever is in the FIFO is dispatched before the next request goes
 * out, and the ESP32 always answers in order. TX still uses the 16550.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "esp32link.h"
#include "znic.h"
#include "netprof.h"
#include "screen.h"
#include "netcfg.h"
#include "../../common/zfsapp.h"	/* fs_write_file() -- NET.IP on the SD */

#define UART1_LSR_DR    0x01
#define UART1_LSR_THRE  0x20
#define UART1_BAUD_DIV  1	/* 48 MHz / 1 / 16 = 3 Mbaud -- the 16550's ceiling,
				   and rtl/esp32_rxfifo.v samples at CLK_PER_BIT 16 */

#define TICKS_PER_SEC   732
#define LINK_TIMEOUT    (40 * TICKS_PER_SEC)
#define HELLO_BURST     (3 * TICKS_PER_SEC)	/* a HELLO later than this
						   after the first one = the
						   ESP32 rebooted */
#define PROBE_INTERVAL  (TICKS_PER_SEC / 20)	/* ~50 ms between polls
						   while no HELLO yet */
#define STA_RETRY_GAP   (1 * TICKS_PER_SEC)
#define STA_MAX_TRIES   5

/* rtl/esp32_rxfifo.v */
#define reg_esp32rx_count (*(volatile uint32_t*)0xf0000300)
#define reg_esp32rx_data  (*(volatile uint32_t*)0xf0000304)
#define reg_esp32rx_flush (*(volatile uint32_t*)0xf0000308)

#define REPLY_TICKS     30	/* ~40 ms for the ESP32 to start answering
				   (normally < 1 ms; tcpip/wifi can delay it) */

/* ---- how often to poll ---------------------------------------------
 *
 * The ESP32 answers, it does not speak first -- with two exceptions,
 * and they are the ones that decide this. Browser keyboard and mouse
 * events are pushed the moment they arrive (screend.c's ws_handler
 * calls znic_send directly, not the poll queue), and so is DATA_ACK.
 * Both land in the BRAM receive FIFO on their own, and an arrival there
 * raises Z_IRQ_ETH, which the kernel turns into an unblock of net0. So
 * the interactive path -- the one that made this loop poll fast in the
 * first place -- does not need a poll at all.
 *
 * What does still wait for an RX_POLL is an incoming LAN frame and the
 * queued control messages (HELLO, LINK, LOG). Polling every kernel tick
 * for those meant 732 round trips a second, nearly all answered NOP,
 * and -- far more expensive than the round trips -- net permanently in
 * the runnable set, which on a round-robin scheduler is a whole share
 * of the CPU taken from whatever is painting.
 *
 * So the poll runs on a deadline that follows the traffic:
 *
 *   - a conversation is live (a DATA frame arrived within the last
 *     LINK_BUSY_HOLD, or the wifi bring-up has not finished): every
 *     tick, exactly as before, so telnet and TFTP keep their latency
 *     and their throughput;
 *   - otherwise the same 100 ms backstop the wired backends use.
 *
 * Only the FIRST inbound packet after two seconds of silence pays that
 * backstop, because it is itself what switches this back to fast. A
 * ping to an idle board can therefore show one slow reply and then a
 * run of quick ones; that is the whole visible cost. */
#define POLL_GAP_BUSY   1
#define POLL_GAP_IDLE   (TICKS_PER_SEC / 10)
#define LINK_BUSY_HOLD  (2 * TICKS_PER_SEC)
#define PROBE_TICKS     4	/* while the ESP32 may still be booting */
#define BYTE_TICKS      3	/* ~4 ms between bytes of one frame (10 us
				   apart on the wire) */

#define ESP32_CTL_EN    0x1
#define ESP32_CTL_GPIO0 0x2

/* last reply pulled off the wire */
static uint8_t rx_msg[ZNIC_MAX_PAYLOAD];
static uint16_t rx_msg_len;
static uint8_t rx_msg_type;

/* link state, owned by znic_dispatch() */
static int hello_ok;
static int hello_count;
static uint8_t peer_fw;
static uint8_t peer_rst;
static uint32_t first_hello_tick;
static int sta_acked;
static uint8_t sta_status;
static int link_up;
static int last_rssi;
static uint8_t last_reason;
static uint8_t last_scan;
// Station (LAN) address the ESP32 got, host order; 0 until LINK
// reports up. This is the address to point a LAN-side client at.
static uint32_t sta_ip;
static uint32_t crc_errors;
static uint32_t data_dropped;
static uint32_t polls_sent;
static uint32_t polls_unanswered;
static uint32_t nops_rx;
static uint32_t logs_rx;
static uint32_t data_rx;
static uint32_t late_rx;
static uint32_t fifo_overruns;

/* -- TX window (znic v3) --
 * DATA is no longer stop-and-wait: up to ZNIC_TX_WINDOW frames may be
 * in flight, each still answered by a DATA_ACK (dispatched wherever it
 * happens to be read). A full window waits briefly for ack progress;
 * on timeout the count resets and the frames are presumed lost, which
 * is what an ethernet link is allowed to do -- TCP/TFTP retransmit. */
#define ZNIC_TX_WINDOW 4
static uint32_t tx_outstanding;
static uint32_t tx_lost;
static uint32_t bursts_rx;
static uint8_t peer_mac[6];

/* small DATA ring: filled by znic_dispatch(), drained by
 * esp32link_recv(). More than one slot because late replies drained
 * ahead of a request can carry several frames. */
#define RX_RING 16
static uint8_t rx_data[RX_RING][ZNIC_MAX_PAYLOAD];
static uint16_t rx_data_len[RX_RING];
static int rx_head, rx_tail, rx_count;
#define rx_data_pending (rx_count > 0)

/* wifi bring-up state machine, driven by esp32link_poll_wifi() */
enum { PH_HELLO = 0, PH_SEND_STA, PH_WAIT_LINK, PH_DONE };
static int phase;
static uint32_t sta_sent_tick;
static int sta_tries;
static int link_timeout_reported;
static uint32_t last_poll_tick;
static uint32_t link_busy_tick;
static int poll_productive;	/* last reply was not NOP: ask again at once */

/* visor / USB mouse coexistence (reg_vmouse bit 24).
 * net used to OR present on every ZNIC_MOUSE and never clear it, so a
 * single visor packet froze the USB pointer until the FPGA was reset.
 * Last writer wins: a visor packet takes the sprite, visor silence of
 * ~1 s (tab closed, pointer left the canvas, synthetic mouse stopped)
 * drops present so the USB mux in rtl/sysctl.v wins without a wiggle.
 * wm also clears present on a USB move. buttons bit 7 is an explicit
 * leave from the visor page. */
#define VMOUSE_HOLD_TICKS  TICKS_PER_SEC
static uint32_t vmouse_last_tick;
static int vmouse_held;
static uint32_t input_events;	/* ZNIC_MOUSE + ZNIC_INPUT dispatched */

static void vmouse_release(void)
{
	reg_vmouse = 0;
	vmouse_held = 0;
	/* wm sleeps until HID or this poke — dropping present must
	 * wake it so a held button is not stuck down. */
	z_wm_wake();
}

static void vmouse_release_if_stale(void)
{
	if (!vmouse_held)
		return;
	if ((int32_t)(z_uptime_ticks() - vmouse_last_tick) < (int32_t)VMOUSE_HOLD_TICKS)
		return;
	vmouse_release();
}

/* ---- is anybody watching? ------------------------------------------
 *
 * screen.c must not hash and ship the framebuffer to a gateway nobody
 * is reading it from, and net cannot see the WebSocket: it terminates
 * on the ESP32, in screend.c. So screend says how many clients it has,
 * in a ZNIC_VIEWERS message, every time the number changes and once a
 * second regardless. A lost message, or a net started while a browser
 * was already watching, is therefore right again within a second.
 *
 * Firmware older than that message has only its log to go by: every
 * ESP_LOG line crosses the link as ZNIC_LOG, and screend logs "viewer
 * connected (fd N)" and "viewer gone (fd N)". That is a string match on
 * another program's messages, not a contract -- and before the ESP32
 * firmware of task 0043 the keepalive freed idle clients without
 * logging the "gone", so a tab closed on a still screen left net
 * streaming to nobody. The text is still read, but only until the first
 * ZNIC_VIEWERS after each HELLO says the firmware knows better, so an
 * old ESP32 under this net behaves exactly as before.
 *
 * Under both there is a floor: any keyboard or mouse event from the
 * browser also counts as a viewer for the next VIEWER_INPUT_HOLD. A
 * missed viewer therefore means a picture that is frozen until the
 * pointer moves, never one that stays frozen.
 */
#define VIEWER_INPUT_HOLD  (30 * TICKS_PER_SEC)
#define VIEWER_MAX         8	/* screend.c's MAX_CLIENTS */
/* The fds themselves, not a count. screend hands the same fd back to the
 * next viewer (every reconnect in a measurement run said "fd 59"), and a
 * browser that closes cleanly produces no "gone" line at all -- its slot is
 * freed a second later when the keepalive to it fails. Counting up on
 * connect and down on gone therefore drifted upwards, one per reconnect,
 * until net believed somebody was always watching. Holding the fds makes a
 * repeat of one we already have a no-op, which is what it is. */
static int viewer_fd[VIEWER_MAX];
static int viewers;
/* a ZNIC_VIEWERS has arrived since the last HELLO: `viewers` is its n
 * and viewer_fd[] is no longer kept */
static int viewers_counted;
static uint32_t viewer_input_tick;
static int viewer_input_seen;

static void viewer_add(int fd)
{
	for (int i = 0; i < viewers; i++)
		if (viewer_fd[i] == fd)
			return;
	if (viewers < VIEWER_MAX)
		viewer_fd[viewers++] = fd;
}

static void viewer_del(int fd)
{
	for (int i = 0; i < viewers; i++) {
		if (viewer_fd[i] != fd)
			continue;
		viewer_fd[i] = viewer_fd[--viewers];
		return;
	}
}

static void viewer_input(void)
{
	viewer_input_tick = z_uptime_ticks();
	viewer_input_seen = 1;
}

static int viewer_present(void)
{
	if (viewers)
		return 1;
	if (!viewer_input_seen)
		return 0;
	if ((z_uptime_ticks() - viewer_input_tick) < VIEWER_INPUT_HOLD)
		return 1;
	viewer_input_seen = 0;
	return 0;
}

/* Offset just past `needle` in a non-terminated payload, or -1. */
static int msg_find(const uint8_t *h, uint16_t n, const char *needle)
{
	uint16_t m = (uint16_t)strlen(needle);
	if (!m || n < m)
		return -1;
	for (uint16_t i = 0; i + m <= n; i++)
		if (!memcmp(h + i, needle, m))
			return (int)(i + m);
	return -1;
}

/* the "(fd 59)" that screend puts at the end of both of its lines */
static int msg_fd(const uint8_t *h, uint16_t n, int from)
{
	int i = msg_find(h + from, (uint16_t)(n - from), "fd ");
	if (i < 0)
		return -1;
	i += from;
	int v = 0, got = 0;
	for (; i < n && h[i] >= '0' && h[i] <= '9'; i++, got = 1)
		v = v * 10 + (h[i] - '0');
	return got ? v : -1;
}

/* ---- RX FIFO (BRAM) ---------------------------------------------- */

static inline uint32_t rx_avail(void)
{
	return reg_esp32rx_count & 0x3fff;
}

static inline int rx_overrun(void)
{
	return (reg_esp32rx_count >> 12) & 1;
}

/* Wait until the FIFO has a byte, or `ticks` kernel ticks pass.
 *
 * z_uptime_ticks() is a syscall (sw/common/zeitlos.c), and calling it
 * on every turn of a busy-wait was an early bug: each one enters the
 * kernel and can cost up to a tick to return, so a wait of a few dozen
 * iterations turned into tens of milliseconds, dropping net's poll loop
 * to ~16Hz and the remote desktop to ~1s of lag. Hence the short pure
 * MMIO burst before any syscall.
 *
 * But the burst used to be the WHOLE wait: 2048 MMIO reads between
 * clock checks, repeated for up to REPLY_TICKS. A poll's round trip is
 * the ESP32's task-switch plus a couple of ZNIC frames on the wire --
 * a few hundred microseconds in which net held the CPU and did nothing
 * with it. On a round-robin scheduler that time comes straight out of
 * whatever is painting: measured at 80-120us per character cell of a
 * terminal redraw.
 *
 * It does not have to. rtl/esp32_rxfifo.v raises rx_ready the instant
 * the first byte of the reply lands, rtl/sysctl.v edge-detects that
 * into the cpu_irq[8] pulse, and the kernel's Z_IRQ_ETH handler
 * unblocks net0 (sw/os/kernel.c) -- recording the wakeup in
 * Z_PROC_FLAG_WAKE if it arrives before we are actually blocked, so it
 * cannot be lost. So: a short spin catches a reply already on the wire
 * without paying a syscall, and anything longer becomes z_proc_wait(1)
 * -- out of the runnable set, woken by the arrival itself rather than
 * by the timeout.
 *
 * The spin stays as the floor rather than being removed: z_proc_wait()
 * returns immediately when a message is pending in net's mailbox (it
 * must -- otherwise the message would sit unread), and without the
 * MMIO burst in front of it this loop would become a syscall storm in
 * exactly that case.
 */
#define SPINS 256
static int rx_wait(uint32_t ticks)
{
	uint32_t start = z_uptime_ticks();
	for (;;) {
		for (uint32_t n = 0; n < SPINS; n++)
			if (rx_avail())
				return 1;
		if (z_uptime_ticks() - start >= ticks)
			return rx_avail() != 0;
		z_proc_wait(1);
	}
}

/* one byte within `ticks` kernel ticks; data-ready is checked before
 * the clock so the fast path is a single MMIO read */
static int rx_getc_ticks(uint8_t *c, uint32_t ticks)
{
	if (!rx_avail() && !rx_wait(ticks))
		return 0;
	*c = (uint8_t)reg_esp32rx_data;
	return 1;
}

/* 16550 TX FIFO is 16 bytes; THRE means the FIFO is empty, so fill it
 * in one go instead of waiting for empty after every byte. A drag
 * stripe is ~400-800 compressed bytes; this is the 968 cyc/byte half
 * of screen_poll. */
#define UART1_TX_FIFO 16
static void uart1_write(const uint8_t *p, unsigned n)
{
	unsigned bytes = n, chunks = 0, spin = 0;
	(void)bytes; (void)chunks; (void)spin;
	while (n) {
		while ((reg_uart1_lsr & UART1_LSR_THRE) == 0)
			spin++;
		unsigned chunk = n < UART1_TX_FIFO ? n : UART1_TX_FIFO;
		for (unsigned i = 0; i < chunk; i++)
			reg_uart1_data = p[i];
		p += chunk;
		n -= chunk;
		chunks++;
	}
	NP_UART(bytes, chunks, spin);
}

/* CRC-16/CCITT (poly 0x1021, init 0xFFFF) matching znic_crc16() in
 * znic.h, table-driven so a 500-byte DATA frame is 500 lookups instead
 * of 4000 bit loops. Verified off the board against the bit version. */
static const uint16_t crc16tab[256] = {
	0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50a5, 0x60c6, 0x70e7,
	0x8108, 0x9129, 0xa14a, 0xb16b, 0xc18c, 0xd1ad, 0xe1ce, 0xf1ef,
	0x1231, 0x0210, 0x3273, 0x2252, 0x52b5, 0x4294, 0x72f7, 0x62d6,
	0x9339, 0x8318, 0xb37b, 0xa35a, 0xd3bd, 0xc39c, 0xf3ff, 0xe3de,
	0x2462, 0x3443, 0x0420, 0x1401, 0x64e6, 0x74c7, 0x44a4, 0x5485,
	0xa56a, 0xb54b, 0x8528, 0x9509, 0xe5ee, 0xf5cf, 0xc5ac, 0xd58d,
	0x3653, 0x2672, 0x1611, 0x0630, 0x76d7, 0x66f6, 0x5695, 0x46b4,
	0xb75b, 0xa77a, 0x9719, 0x8738, 0xf7df, 0xe7fe, 0xd79d, 0xc7bc,
	0x48c4, 0x58e5, 0x6886, 0x78a7, 0x0840, 0x1861, 0x2802, 0x3823,
	0xc9cc, 0xd9ed, 0xe98e, 0xf9af, 0x8948, 0x9969, 0xa90a, 0xb92b,
	0x5af5, 0x4ad4, 0x7ab7, 0x6a96, 0x1a71, 0x0a50, 0x3a33, 0x2a12,
	0xdbfd, 0xcbdc, 0xfbbf, 0xeb9e, 0x9b79, 0x8b58, 0xbb3b, 0xab1a,
	0x6ca6, 0x7c87, 0x4ce4, 0x5cc5, 0x2c22, 0x3c03, 0x0c60, 0x1c41,
	0xedae, 0xfd8f, 0xcdec, 0xddcd, 0xad2a, 0xbd0b, 0x8d68, 0x9d49,
	0x7e97, 0x6eb6, 0x5ed5, 0x4ef4, 0x3e13, 0x2e32, 0x1e51, 0x0e70,
	0xff9f, 0xefbe, 0xdfdd, 0xcffc, 0xbf1b, 0xaf3a, 0x9f59, 0x8f78,
	0x9188, 0x81a9, 0xb1ca, 0xa1eb, 0xd10c, 0xc12d, 0xf14e, 0xe16f,
	0x1080, 0x00a1, 0x30c2, 0x20e3, 0x5004, 0x4025, 0x7046, 0x6067,
	0x83b9, 0x9398, 0xa3fb, 0xb3da, 0xc33d, 0xd31c, 0xe37f, 0xf35e,
	0x02b1, 0x1290, 0x22f3, 0x32d2, 0x4235, 0x5214, 0x6277, 0x7256,
	0xb5ea, 0xa5cb, 0x95a8, 0x8589, 0xf56e, 0xe54f, 0xd52c, 0xc50d,
	0x34e2, 0x24c3, 0x14a0, 0x0481, 0x7466, 0x6447, 0x5424, 0x4405,
	0xa7db, 0xb7fa, 0x8799, 0x97b8, 0xe75f, 0xf77e, 0xc71d, 0xd73c,
	0x26d3, 0x36f2, 0x0691, 0x16b0, 0x6657, 0x7676, 0x4615, 0x5634,
	0xd94c, 0xc96d, 0xf90e, 0xe92f, 0x99c8, 0x89e9, 0xb98a, 0xa9ab,
	0x5844, 0x4865, 0x7806, 0x6827, 0x18c0, 0x08e1, 0x3882, 0x28a3,
	0xcb7d, 0xdb5c, 0xeb3f, 0xfb1e, 0x8bf9, 0x9bd8, 0xabbb, 0xbb9a,
	0x4a75, 0x5a54, 0x6a37, 0x7a16, 0x0af1, 0x1ad0, 0x2ab3, 0x3a92,
	0xfd2e, 0xed0f, 0xdd6c, 0xcd4d, 0xbdaa, 0xad8b, 0x9de8, 0x8dc9,
	0x7c26, 0x6c07, 0x5c64, 0x4c45, 0x3ca2, 0x2c83, 0x1ce0, 0x0cc1,
	0xef1f, 0xff3e, 0xcf5d, 0xdf7c, 0xaf9b, 0xbfba, 0x8fd9, 0x9ff8,
	0x6e17, 0x7e36, 0x4e55, 0x5e74, 0x2e93, 0x3eb2, 0x0ed1, 0x1ef0,
};

static uint16_t crc16_add(uint16_t crc, const uint8_t *p, unsigned n)
{
	while (n--)
		crc = (uint16_t)((crc << 8) ^ crc16tab[((crc >> 8) ^ *p++) & 0xff]);
	return crc;
}

static void uart1_init(void)
{
	reg_uart1_lcr = 0x83;	/* DLAB, 8N1 */
	reg_uart1_dlbh = 0;
	reg_uart1_dlbl = UART1_BAUD_DIV;
	reg_uart1_lcr = 0x03;	/* 8N1 */
	reg_uart1_fcr = 0x07;	/* FIFO on, flush both, trigger 1 */
	reg_uart1_ier = 0x00;	/* poll, no IRQ */
}

/* ---- ZNIC framing ------------------------------------------------ */

static void znic_tx_raw(uint8_t type, const uint8_t *payload, uint16_t n)
{
	uint8_t start[6];
	start[0] = ZNIC_SYNC0;
	start[1] = ZNIC_SYNC1;
	start[2] = ZNIC_VER;
	start[3] = type;
	start[4] = (uint8_t)(n & 0xFF);
	start[5] = (uint8_t)(n >> 8);
	uint16_t crc = crc16_add(0xFFFF, start + 2, 4);
	if (n && payload)
		crc = crc16_add(crc, payload, n);
	uint8_t tail[2] = { (uint8_t)(crc & 0xFF), (uint8_t)(crc >> 8) };
	uart1_write(start, 6);
	if (n && payload)
		uart1_write(payload, n);
	uart1_write(tail, 2);
}

/* one framed message: sync hunt bounded by `first` ticks, then every
 * byte within BYTE_TICKS. Returns 1 with rx_msg_* filled. */
static int znic_rx_raw(uint32_t first)
{
	uint8_t c;
	uint32_t start = z_uptime_ticks();
	int seen7e = 0;
	for (;;) {
		if (!rx_avail()) {
			uint32_t gone = z_uptime_ticks() - start;
			if (gone >= first || !rx_wait(first - gone))
				return 0;
		}
		c = (uint8_t)reg_esp32rx_data;
		if (!seen7e) {
			if (c == ZNIC_SYNC0)
				seen7e = 1;
			continue;
		}
		if (c != ZNIC_SYNC1) {
			seen7e = (c == ZNIC_SYNC0);
			continue;
		}
		break;
	}

	uint8_t hdr[4];
	for (int i = 0; i < 4; i++) {
		if (!rx_getc_ticks(&hdr[i], BYTE_TICKS))
			return 0;
	}
	uint16_t n = (uint16_t)hdr[2] | ((uint16_t)hdr[3] << 8);
	if (hdr[0] != ZNIC_VER || n > ZNIC_MAX_PAYLOAD) {
		crc_errors++;
		return 0;
	}
	for (uint16_t i = 0; i < n; i++) {
		if (!rx_getc_ticks(&rx_msg[i], BYTE_TICKS))
			return 0;
	}
	uint8_t crcl, crch;
	if (!rx_getc_ticks(&crcl, BYTE_TICKS) || !rx_getc_ticks(&crch, BYTE_TICKS))
		return 0;
	uint16_t got = (uint16_t)crcl | ((uint16_t)crch << 8);

	uint16_t cacc = znic_crc16(hdr, 4);
	for (uint16_t i = 0; i < n; i++) {
		cacc ^= (uint16_t)rx_msg[i] << 8;
		for (int b = 0; b < 8; b++)
			cacc = (cacc & 0x8000) ? (uint16_t)((cacc << 1) ^ 0x1021)
			                       : (uint16_t)(cacc << 1);
	}
	if (cacc != got) {
		crc_errors++;
		return 0;
	}
	rx_msg_type = hdr[1];
	rx_msg_len = n;
	return 1;
}

static void znic_dispatch(void);

/* whatever already sits in the FIFO is a late reply to an earlier
 * request: dispatch it, never drop it */
static void znic_drain(void)
{
	if (rx_overrun()) {
		fifo_overruns++;
		reg_esp32rx_flush = 1;	/* stream is broken anyway; clears the flag */
		return;
	}
	while (rx_avail()) {
		if (!znic_rx_raw(1))
			break;
		late_rx++;
		znic_dispatch();
	}
}

/* One transaction: drain late replies, send our frame, read one
 * reply. With `want` != 0, keep reading (dispatching the others)
 * until that type shows up. Returns the reply type or -1 on timeout.
 * Interrupts stay enabled: the BRAM FIFO holds what arrives while
 * another process runs. */
static int znic_xfer(uint8_t type, const uint8_t *payload, uint16_t n,
	uint32_t first, uint8_t want)
{
	znic_drain();
	znic_tx_raw(type, payload, n);
	uint32_t start = z_uptime_ticks();
	for (;;) {
		uint32_t left = first - (z_uptime_ticks() - start);
		if (z_uptime_ticks() - start >= first)
			return -1;
		if (!znic_rx_raw(left ? left : 1))
			return -1;
		if (!want || rx_msg_type == want)
			return (int)rx_msg_type;
		late_rx++;
		znic_dispatch();	/* a straggler; keep waiting for ours */
	}
}

/* apply the reply in rx_msg_* to the link state (prints allowed) */
static void znic_dispatch(void)
{
	switch (rx_msg_type) {

	case ZNIC_HELLO:
		hello_count++;
		if (rx_msg_len >= 6)
			memcpy(peer_mac, rx_msg, 6);
		/* flags byte = esp_reset_reason(): 1 poweron, 3 sw, 4 panic,
		 * 5 int wdt, 6 task wdt, 7 wdt, 9 brownout */
		peer_rst = (rx_msg_len >= 7) ? rx_msg[6] : 0;
		peer_fw = (rx_msg_len >= 8) ? rx_msg[7] : 0;
		if (!hello_ok) {
			hello_ok = 1;
			first_hello_tick = z_uptime_ticks();
			printf("esp32link: HELLO fw=%u rst=%u mac %02x:%02x:%02x:%02x:%02x:%02x\n",
				peer_fw, peer_rst,
				peer_mac[0], peer_mac[1], peer_mac[2],
				peer_mac[3], peer_mac[4], peer_mac[5]);
		} else if (z_uptime_ticks() - first_hello_tick > HELLO_BURST) {
			printf("esp32link: HELLO again (#%d, %lus after the first, rst=%u) "
				"-- ESP32 reset? resending STA\n", hello_count,
				(unsigned long)((z_uptime_ticks() - first_hello_tick)
					/ TICKS_PER_SEC), peer_rst);
			first_hello_tick = z_uptime_ticks();
			link_up = 0;
			sta_acked = 0;
			sta_tries = 0;
			if (phase != PH_HELLO)
				phase = PH_SEND_STA;
		}
				/* a fresh HELLO means the ESP32 (re)booted: its screen shadow
		 * is empty and its WebSocket clients are gone, so forget them
		 * and resend the whole framebuffer once one comes back */
		viewers = 0;
		viewers_counted = 0;
		screen_reset();
		break;

	case ZNIC_STA_ACK:
		sta_acked = 1;
		sta_status = (rx_msg_len >= 1) ? rx_msg[0] : 0xff;
		printf("esp32link: STA_ACK status %u\n", sta_status);
		break;

	case ZNIC_LINK:
		if (rx_msg_len >= 1)
			link_up = rx_msg[0] ? 1 : 0;
		if (rx_msg_len >= 2)
			last_rssi = (int8_t)rx_msg[1];
		if (rx_msg_len >= 3)
			last_reason = rx_msg[2];
		if (rx_msg_len >= 4)
			last_scan = rx_msg[3];
		if (rx_msg_len >= 8)
			sta_ip = ((uint32_t)rx_msg[4] << 24) | ((uint32_t)rx_msg[5] << 16)
				| ((uint32_t)rx_msg[6] << 8) | rx_msg[7];
		if (link_up)
			printf("esp32link: LINK up rssi=%d ip=%lu.%lu.%lu.%lu\n",
				last_rssi, (sta_ip >> 24) & 0xff, (sta_ip >> 16) & 0xff,
				(sta_ip >> 8) & 0xff, sta_ip & 0xff);
		else
			printf("esp32link: LINK down reason=%u scan=%u\n",
				last_reason, last_scan);
		/* Leave the DHCP address on the SD so a headless board (no
		 * HDMI, no serial console) can still be found: pull the card
		 * and read NET.IP. LINK can repeat as a poll reply, so only
		 * rewrite on an actual state change -- never on every poll. */
		{
			static uint32_t ip_on_card = 0;
			static int up_on_card = -1;
			if (link_up != up_on_card ||
			    (link_up && sta_ip != ip_on_card)) {
				char line[24];
				int n;
				if (link_up)
					n = snprintf(line, sizeof line,
						"%lu.%lu.%lu.%lu\n",
						(sta_ip >> 24) & 0xff, (sta_ip >> 16) & 0xff,
						(sta_ip >> 8) & 0xff, sta_ip & 0xff);
				else
					n = snprintf(line, sizeof line, "down\n");
				if (n > 0)
					fs_write_file("NET.IP", line, n);
				ip_on_card = sta_ip;
				up_on_card = link_up;
			}
		}
		if (phase == PH_WAIT_LINK)
			phase = PH_DONE;
		break;

	case ZNIC_LOG:	/* one ESP_LOG line from the firmware */
		logs_rx++;
		printf("esp32: %.*s\n", (int)rx_msg_len, (const char *)rx_msg);
		/* firmware without ZNIC_VIEWERS announces its WebSocket
		 * clients only here -- see viewer_present() above */
		{
			int at = msg_find(rx_msg, rx_msg_len, "viewer connected");
			if (at >= 0) {
				if (!viewers_counted)
					viewer_add(msg_fd(rx_msg, rx_msg_len, at));
				/* its shadow is whatever we last streamed, which may
				 * be nothing at all: start from a whole frame */
				screen_reset();
			} else if (!viewers_counted &&
					(at = msg_find(rx_msg, rx_msg_len, "viewer gone")) >= 0) {
				viewer_del(msg_fd(rx_msg, rx_msg_len, at));
			}
		}
		break;

	case ZNIC_VIEWERS:	/* {n:u8}: screend's WebSocket clients, now */
		if (rx_msg_len >= 1) {
			int n = rx_msg[0] < VIEWER_MAX ? rx_msg[0] : VIEWER_MAX;
			/* one more than we knew of, even if its "connected"
			 * line was lost: a whole frame, as above */
			if (n > viewers)
				screen_reset();
			viewers = n;
			viewers_counted = 1;
		}
		break;

	case ZNIC_DATA:
		data_rx++;
		/* a conversation is live -- keep polling every tick until it
		 * has been quiet for LINK_BUSY_HOLD (see poll_gap()) */
		link_busy_tick = z_uptime_ticks();
		if (rx_count < RX_RING) {
			memcpy(rx_data[rx_head], rx_msg, rx_msg_len);
			rx_data_len[rx_head] = rx_msg_len;
			rx_head = (rx_head + 1) % RX_RING;
			rx_count++;
		} else {
			data_dropped++;
		}
		break;

	case ZNIC_NOP:
		nops_rx++;
		break;
	case ZNIC_MOUSE:
		/* {x_lo,x_hi,y_lo,y_hi,buttons} -> the virtual mouse register.
		 * Bit 7 of buttons = pointer left the visor: drop present so
		 * USB is not latched. Otherwise set present (rtl/sysctl.v;
		 * wm reads it like a USB mouse) and refresh the hold timer. */
		if (rx_msg_len >= 5) {
			uint32_t x = (uint32_t)rx_msg[0] | ((uint32_t)rx_msg[1] << 8);
			uint32_t y = (uint32_t)rx_msg[2] | ((uint32_t)rx_msg[3] << 8);
			uint32_t b = rx_msg[4];
			viewer_input();
			if (b & 0x80) {
				vmouse_release();
			} else {
				reg_vmouse = (1u << 24) | ((b & 7) << 20)
					| ((y & 0x3ff) << 10) | (x & 0x3ff);
				vmouse_last_tick = z_uptime_ticks();
				vmouse_held = 1;
				input_events++;
				z_wm_wake();
			}
		}
		break;
	case ZNIC_INPUT:
		if (rx_msg_len >= 3) {
			viewer_input();
			uint32_t ev = (((uint32_t)rx_msg[1] & 0xff) << 9)
				| (((uint32_t)rx_msg[0] & 0xff) << 1)
				| (rx_msg[2] ? 1u : 0u);
			hid_inject((int32_t)ev);
			input_events++;
		}
		break;
	case ZNIC_BURST:
		/* container marker: the frames it announces follow in the
		 * stream and are read by whoever is draining it */
		break;
	case ZNIC_DATA_ACK:
		if (tx_outstanding)
			tx_outstanding--;
		break;

	default:	/* DATA_ACK, unknown */
		break;
	}
}

static int znic_send_sta(const netcfg_t *cfg)
{
	/* One (ssid_len, ssid, psk_len, psk) tuple per configured network,
	 * in NET.CFG order. The firmware scans and takes the first one
	 * that exists; a single-network payload is byte-identical to what
	 * this always sent. */
	static uint8_t body[NETCFG_WIFI_MAX * (2 + NETCFG_SSID_MAX + NETCFG_PSK_MAX)];
	uint16_t off = 0;
	for (int i = 0; i < cfg->n_wifi; i++) {
		uint8_t sl = (uint8_t)strlen(cfg->wifi[i].ssid);
		uint8_t pl = (uint8_t)strlen(cfg->wifi[i].psk);
		body[off++] = sl;
		memcpy(body + off, cfg->wifi[i].ssid, sl);
		off += sl;
		body[off++] = pl;
		memcpy(body + off, cfg->wifi[i].psk, pl);
		off += pl;
	}
	sta_acked = 0;
	sta_sent_tick = z_uptime_ticks();
	sta_tries++;
	int t = znic_xfer(ZNIC_STA, body, off, REPLY_TICKS, ZNIC_STA_ACK);
	printf("esp32link: STA sent, %d network(s) (try %d)\n",
		cfg->n_wifi, sta_tries);
	if (t >= 0)
		znic_dispatch();
	return t;
}

/* ---- PHY API ----------------------------------------------------- */

bool esp32link_init(const uint8_t mac[6])
{
	(void)mac;
	hello_ok = 0;
	hello_count = 0;
	peer_fw = 0;
	peer_rst = 0;
	sta_acked = 0;
	sta_status = 0xff;
	link_up = 0;
	last_rssi = 0;
	last_reason = 0;
	last_scan = 0;
	crc_errors = 0;
	data_dropped = 0;
	polls_sent = 0;
	polls_unanswered = 0;
	nops_rx = 0;
	logs_rx = 0;
	data_rx = 0;
	late_rx = 0;
	fifo_overruns = 0;
	rx_head = rx_tail = rx_count = 0;
	phase = PH_HELLO;
	viewers = 0;
	viewers_counted = 0;
	viewer_input_seen = 0;
	sta_tries = 0;
	link_timeout_reported = 0;
	last_poll_tick = 0;
	link_busy_tick = 0;
	poll_productive = 0;

	uart1_init();
	reg_esp32rx_flush = 1;

	/* gpio0=1, en=0 then en=1 so the ESP32 boots from flash */
	reg_esp32_ctl = ESP32_CTL_GPIO0;
	delay_ms(20);
	reg_esp32_ctl = ESP32_CTL_GPIO0 | ESP32_CTL_EN;

	/* Do not block here: HELLO/STA/LINK are collected by the main
	 * loop's polls. */
	printf("esp32link: ESP32 released, HELLO/STA in main loop\n");
	fflush(stdout);
	return true;
}

int esp32link_hello_ok(void)
{
	return hello_ok;
}

int esp32link_link_is_up(void)
{
	return link_up;
}

void esp32link_poll_wifi(const netcfg_t *cfg)
{
	if (!cfg || !cfg->n_wifi)
		return;

	vmouse_release_if_stale();
	screen_set_viewer(viewer_present());

	switch (phase) {

	case PH_HELLO:	/* HELLO arrives as a poll reply (recv) */
		if (hello_ok)
			phase = PH_SEND_STA;
		break;

	case PH_SEND_STA:
		if (sta_tries && z_uptime_ticks() - sta_sent_tick < STA_RETRY_GAP)
			break;
		if (sta_tries >= STA_MAX_TRIES) {
			printf("esp32link: no STA_ACK after %d tries, giving up\n",
				sta_tries);
			esp32link_debug_dump();
			phase = PH_DONE;
			break;
		}
		if (znic_send_sta(cfg) == ZNIC_STA_ACK) {
			link_timeout_reported = 0;
			phase = PH_WAIT_LINK;
		}
		break;

	case PH_WAIT_LINK:	/* LINK arrives as a poll reply (recv) */
		if (!link_timeout_reported &&
				z_uptime_ticks() - sta_sent_tick > LINK_TIMEOUT) {
			printf("esp32link: no LINK %us after STA (still listening)\n",
				LINK_TIMEOUT / TICKS_PER_SEC);
			esp32link_debug_dump();
			link_timeout_reported = 1;
		}
		break;

	default:	/* PH_DONE */
		break;
	}
}

static uint32_t poll_gap(void)
{
	if (!hello_ok)
		return PROBE_INTERVAL;	/* the ESP32 may still be booting */
	if (phase != PH_DONE)
		return POLL_GAP_BUSY;	/* HELLO/STA_ACK/LINK are poll replies */
	if ((z_uptime_ticks() - link_busy_tick) < LINK_BUSY_HOLD)
		return POLL_GAP_BUSY;
	return POLL_GAP_IDLE;
}

/* Ticks until this link wants the loop again; 0 = now. net's idle wait
 * sleeps on it (with screen_idle_ticks()), so the process is out of the
 * runnable set in between instead of waking 732 times a second. */
uint32_t esp32link_idle_ticks(void)
{
	uint32_t gap, gone;
	if (rx_data_pending || poll_productive)
		return 0;
	gap = poll_gap();
	gone = z_uptime_ticks() - last_poll_tick;
	return gone >= gap ? 0 : gap - gone;
}

uint16_t esp32link_recv(uint8_t *buf, uint16_t maxlen)
{
	/* Whatever the ESP32 pushed on its own -- browser input, DATA_ACK,
	 * a reply that arrived after we stopped waiting for it -- is in the
	 * FIFO already, and this is what the ETH interrupt woke us for.
	 * Costs one MMIO read when there is nothing. */
	znic_drain();

	if (!rx_data_pending && esp32link_idle_ticks() == 0) {
		last_poll_tick = z_uptime_ticks();
		polls_sent++;
		/* how many frames we can take right now: ring slots and FIFO
		 * space, worst-case MTU each. At least 1; more turns the
		 * reply into a BURST. */
		uint32_t room = ((1u << 13) - rx_avail()) / (ZNIC_MAX_PAYLOAD + 8);
		uint32_t slots = RX_RING - rx_count;
		uint8_t credit = (uint8_t)((room < slots ? room : slots));
		if (credit < 1)
			credit = 1;
		if (credit > 15)
			credit = 15;
		int t = znic_xfer(ZNIC_RX_POLL, &credit, 1,
			hello_ok ? REPLY_TICKS : PROBE_TICKS, 0);
		if (t < 0) {
			polls_unanswered++;
			poll_productive = 0;
			return 0;
		}
		/* NOP is the ESP32 saying both its queues are empty. Anything
		 * else means it had something, so there may be more behind it
		 * and the next poll should not wait out a gap -- that is what
		 * keeps a TFTP transfer at wire speed, and what drains a burst
		 * of firmware log lines before the 48-slot queue overflows. */
		poll_productive = 0;
		if (rx_msg_type == ZNIC_BURST) {
			uint8_t n = rx_msg_len >= 1 ? rx_msg[0] : 0;
			bursts_rx++;
			for (uint8_t i = 0; i < n; i++) {
				if (!znic_rx_raw(REPLY_TICKS))
					break;
				if (rx_msg_type != ZNIC_NOP)
					poll_productive = 1;
				znic_dispatch();
			}
		} else {
			poll_productive = (rx_msg_type != ZNIC_NOP);
			znic_dispatch();
		}
	}
	if (!rx_data_pending)
		return 0;

	uint16_t n = rx_data_len[rx_tail];
	if (n > maxlen)
		n = maxlen;
	memcpy(buf, rx_data[rx_tail], n);
	rx_tail = (rx_tail + 1) % RX_RING;
	rx_count--;
	return n;
}

int esp32link_pump_input(void)
{
	uint32_t before = input_events;
	znic_drain();
	return (int)(input_events - before);
}

int esp32link_vmouse_buttons(void)
{
	if (!vmouse_held)
		return 0;
	return (int)((reg_vmouse >> 20) & 7);
}


bool esp32link_send(const uint8_t *buf, uint16_t len)
{
	if (len > ZNIC_MAX_PAYLOAD)
		return false;
	/* Never block net's main loop here: it is the same loop that polls
	 * the link for keyboard/mouse input, and waiting tens of ms for a
	 * DATA_ACK (the old behaviour) dropped that poll rate to ~17Hz and
	 * made the remote desktop lag by ~1s. znic_drain() has already
	 * collected any acks that arrived; if the window is still full the
	 * oldest frames are presumed lost (an ethernet link may drop, and
	 * TCP/TFTP retransmit) and we send immediately. */
	znic_drain();
	if (tx_outstanding >= ZNIC_TX_WINDOW) {
		tx_lost += tx_outstanding;
		tx_outstanding = 0;
	}
	znic_tx_raw(ZNIC_DATA, buf, len);
	tx_outstanding++;
	return true;
}

void esp32link_debug_dump(void)
{
	printf("esp32link: hello=%d(#%d) fw=%u rst=%u sta_ack=%d status=%u link=%d "
		"rssi=%d phase=%d polls=%lu unanswered=%lu nops=%lu logs=%lu "
		"data=%lu late=%lu crc_err=%lu dropped=%lu fifo=%lu ovr=%lu "
		"bursts=%lu txout=%lu txlost=%lu input=%lu ctl=0x%lx\n",
		hello_ok, hello_count, peer_fw, peer_rst, sta_acked, sta_status,
		link_up, last_rssi, phase, (unsigned long)polls_sent,
		(unsigned long)polls_unanswered, (unsigned long)nops_rx,
		(unsigned long)logs_rx, (unsigned long)data_rx,
		(unsigned long)late_rx, (unsigned long)crc_errors,
		(unsigned long)data_dropped, (unsigned long)rx_avail(),
		(unsigned long)fifo_overruns, (unsigned long)bursts_rx,
		(unsigned long)tx_outstanding, (unsigned long)tx_lost,
		(unsigned long)input_events, (unsigned long)reg_esp32_ctl);
}
