/*
 * Zeitlos -- UART PHY to the onboard ESP32 (ULX3S).
 *
 * Third NIC backend (NET_PHY=ESP32LINK). Speaks ZNIC over UART1;
 * 802.11 stays on the ESP32. See docs/esp32-net.md and znic.h.
 *
 * Transport model (fw ver 2): every frame the ESP32 sends is the
 * reply to one frame we sent. RX_POLL is answered with the oldest
 * queued control message (HELLO, LINK, LOG), else a DATA frame, else
 * NOP; DATA is answered with DATA_ACK; STA with STA_ACK. A whole
 * transaction (our frame + the reply) runs with interrupts masked:
 * UART1 is a 16-byte 16550 FIFO read by polling from a time-sliced
 * process, so anything that arrives while another process holds the
 * CPU (up to ~4 ms, i.e. 400 bytes at 1 Mbaud) is lost -- that is how
 * STA_ACK, LINK and ESP32 log lines went missing on 2026-08-27. Same
 * approach as enc28j60.c's maskirq() around SPI transactions; a
 * 1518-byte DATA reply masks for ~16 ms. Timeouts inside the masked
 * window use rdcycle (48 MHz), because the kernel tick is an IRQ.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "esp32link.h"
#include "znic.h"
#include "netcfg.h"

#define UART1_LSR_DR    0x01
#define UART1_LSR_THRE  0x20
#define UART1_BAUD_DIV  3	/* 48 MHz / 3 / 16 = 1 Mbaud, same as UART0 */

#define TICKS_PER_SEC   732
#define LINK_TIMEOUT    (40 * TICKS_PER_SEC)
#define HELLO_BURST     (3 * TICKS_PER_SEC)	/* a HELLO later than this
						   after the first one = the
						   ESP32 rebooted */
#define PROBE_INTERVAL  (TICKS_PER_SEC / 20)	/* ~50 ms between polls
						   while no HELLO yet */
#define STA_RETRY_GAP   (1 * TICKS_PER_SEC)
#define STA_MAX_TRIES   5

#define CYC_PER_MS      48000u
#define REPLY_CYC       (8 * CYC_PER_MS)	/* ESP32 task wake-up + first
						   byte, normally < 1 ms */
#define PROBE_CYC       (2 * CYC_PER_MS)	/* while the ESP32 may still be
						   booting: keep the masked
						   wait short */
#define BYTE_CYC        (1 * CYC_PER_MS)	/* between bytes of one frame
						   (10 us apart on the wire) */

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
static uint32_t crc_errors;
static uint32_t data_dropped;
static uint32_t polls_sent;
static uint32_t polls_unanswered;
static uint32_t nops_rx;
static uint32_t logs_rx;
static uint8_t peer_mac[6];

/* one-slot DATA queue: filled by znic_dispatch(), drained by
 * esp32link_recv() */
static uint8_t rx_data[ZNIC_MAX_PAYLOAD];
static uint16_t rx_data_len;
static int rx_data_pending;

/* wifi bring-up state machine, driven by esp32link_poll_wifi() */
enum { PH_HELLO = 0, PH_SEND_STA, PH_WAIT_LINK, PH_DONE };
static int phase;
static uint32_t sta_sent_tick;
static int sta_tries;
static int link_timeout_reported;
static uint32_t last_probe_tick;

/* ---- cycle counter (immune to maskirq, unlike z_uptime_ticks) ---- */

static inline uint32_t cyc(void)
{
	uint32_t x;
	__asm__ volatile (".option push\n.option arch, +zicsr\n"
		"rdcycle %0\n.option pop" : "=r"(x));
	return x;
}

/* ---- UART1 ------------------------------------------------------- */

static inline int uart1_rx_ready(void)
{
	return (reg_uart1_lsr & UART1_LSR_DR) != 0;
}

/* one byte within `limit` cycles; data-ready is checked before the
 * counter so the fast path is a single MMIO read */
static int uart1_getc_cyc(uint8_t *c, uint32_t limit)
{
	uint32_t start = cyc();
	for (;;) {
		if (uart1_rx_ready()) {
			*c = (uint8_t)reg_uart1_data;
			return 1;
		}
		if (cyc() - start >= limit)
			return 0;
	}
}

static void uart1_putc(uint8_t c)
{
	while ((reg_uart1_lsr & UART1_LSR_THRE) == 0)
		;
	reg_uart1_data = c;
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
	uint8_t hdr[4];
	hdr[0] = ZNIC_VER;
	hdr[1] = type;
	hdr[2] = (uint8_t)(n & 0xFF);
	hdr[3] = (uint8_t)(n >> 8);
	uint16_t crc = znic_crc16(hdr, 4);
	if (n && payload) {
		/* continue the CRC over the payload without copying it */
		for (uint16_t i = 0; i < n; i++) {
			crc ^= (uint16_t)payload[i] << 8;
			for (int b = 0; b < 8; b++)
				crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
				                     : (uint16_t)(crc << 1);
		}
	}
	uart1_putc(ZNIC_SYNC0);
	uart1_putc(ZNIC_SYNC1);
	for (int i = 0; i < 4; i++)
		uart1_putc(hdr[i]);
	for (uint16_t i = 0; i < n; i++)
		uart1_putc(payload[i]);
	uart1_putc((uint8_t)(crc & 0xFF));
	uart1_putc((uint8_t)(crc >> 8));
}

/* one framed message: sync hunt bounded by `first` cycles, then every
 * byte within BYTE_CYC. Returns 1 with rx_msg_* filled. */
static int znic_rx_raw(uint32_t first)
{
	uint8_t c;
	uint32_t start = cyc();
	int seen7e = 0;
	for (;;) {
		if (!uart1_rx_ready()) {
			if (cyc() - start >= first)
				return 0;
			continue;
		}
		c = (uint8_t)reg_uart1_data;
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
		if (!uart1_getc_cyc(&hdr[i], BYTE_CYC))
			return 0;
	}
	uint16_t n = (uint16_t)hdr[2] | ((uint16_t)hdr[3] << 8);
	if (hdr[0] != ZNIC_VER || n > ZNIC_MAX_PAYLOAD) {
		crc_errors++;
		return 0;
	}
	for (uint16_t i = 0; i < n; i++) {
		if (!uart1_getc_cyc(&rx_msg[i], BYTE_CYC))
			return 0;
	}
	uint8_t crcl, crch;
	if (!uart1_getc_cyc(&crcl, BYTE_CYC) || !uart1_getc_cyc(&crch, BYTE_CYC))
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

/* One transaction, interrupts masked throughout: drop stale RX bytes,
 * send our frame, read exactly one reply. Returns the reply type or
 * -1. Nothing here prints; callers dispatch afterwards. */
static int znic_xfer(uint8_t type, const uint8_t *payload, uint16_t n,
	uint32_t first)
{
	uint32_t old = maskirq(0xFFFFFFFF);
	reg_uart1_fcr = 0x03;	/* FIFO on + reset RX FIFO */
	znic_tx_raw(type, payload, n);
	int ok = znic_rx_raw(first);
	maskirq(old);
	return ok ? (int)rx_msg_type : -1;
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
		if (link_up)
			printf("esp32link: LINK up rssi=%d\n", last_rssi);
		else
			printf("esp32link: LINK down reason=%u scan=%u\n",
				last_reason, last_scan);
		if (phase == PH_WAIT_LINK)
			phase = PH_DONE;
		break;

	case ZNIC_LOG:	/* one ESP_LOG line from the firmware */
		logs_rx++;
		printf("esp32: %.*s\n", (int)rx_msg_len, (const char *)rx_msg);
		break;

	case ZNIC_DATA:
		if (!rx_data_pending) {
			memcpy(rx_data, rx_msg, rx_msg_len);
			rx_data_len = rx_msg_len;
			rx_data_pending = 1;
		} else {
			data_dropped++;
		}
		break;

	case ZNIC_NOP:
		nops_rx++;
		break;

	default:	/* DATA_ACK, unknown */
		break;
	}
}

static int znic_send_sta(const char *ssid, const char *psk)
{
	uint8_t body[1 + NETCFG_SSID_MAX + 1 + NETCFG_PSK_MAX];
	uint8_t sl = (uint8_t)strlen(ssid);
	uint8_t pl = (uint8_t)strlen(psk ? psk : "");
	if (sl > NETCFG_SSID_MAX)
		sl = NETCFG_SSID_MAX;
	if (pl > NETCFG_PSK_MAX)
		pl = NETCFG_PSK_MAX;
	body[0] = sl;
	memcpy(body + 1, ssid, sl);
	body[1 + sl] = pl;
	if (pl)
		memcpy(body + 2 + sl, psk, pl);
	sta_acked = 0;
	sta_sent_tick = z_uptime_ticks();
	sta_tries++;
	int t = znic_xfer(ZNIC_STA, body, (uint16_t)(2 + sl + pl), REPLY_CYC);
	printf("esp32link: STA sent ssid='%s' (try %d)\n", ssid, sta_tries);
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
	rx_data_pending = 0;
	phase = PH_HELLO;
	sta_tries = 0;
	link_timeout_reported = 0;
	last_probe_tick = 0;

	/* self-check of the cycle counter the masked timeouts rely on:
	 * 10 ms of wall clock should be ~480000 cycles at 48 MHz */
	uint32_t c0 = cyc();
	delay_ms(10);
	uint32_t c1 = cyc();
	printf("esp32link: rdcycle %lu cycles / 10 ms\n", (unsigned long)(c1 - c0));

	uart1_init();

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

void esp32link_poll_wifi(const char *ssid, const char *psk)
{
	if (!ssid || !ssid[0])
		return;

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
		if (znic_send_sta(ssid, psk) == ZNIC_STA_ACK) {
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

/* Blocking variant (not used by net.c's ESP32LINK build, kept for the
 * phy_wifi_sta contract). */
bool esp32link_wifi_sta(const char *ssid, const char *psk)
{
	uint8_t tmp[ZNIC_MAX_PAYLOAD];
	uint32_t start = z_uptime_ticks();
	while (!hello_ok && z_uptime_ticks() - start < 5 * TICKS_PER_SEC)
		esp32link_recv(tmp, sizeof(tmp));
	if (!hello_ok) {
		printf("esp32link: no HELLO\n");
		return false;
	}
	sta_tries = 0;
	if (znic_send_sta(ssid, psk) != ZNIC_STA_ACK || sta_status != ZNIC_STA_OK) {
		printf("esp32link: no STA_ACK\n");
		return false;
	}
	printf("esp32link: waiting for LINK...\n");
	phase = PH_WAIT_LINK;
	start = z_uptime_ticks();
	while (phase == PH_WAIT_LINK && z_uptime_ticks() - start < LINK_TIMEOUT)
		esp32link_recv(tmp, sizeof(tmp));
	if (phase == PH_WAIT_LINK) {
		printf("esp32link: no LINK (association timed out)\n");
		return false;
	}
	return link_up != 0;
}

uint16_t esp32link_recv(uint8_t *buf, uint16_t maxlen)
{
	if (!rx_data_pending) {
		/* before HELLO the ESP32 may still be booting: probe gently */
		if (!hello_ok) {
			if (z_uptime_ticks() - last_probe_tick < PROBE_INTERVAL)
				return 0;
			last_probe_tick = z_uptime_ticks();
		}
		polls_sent++;
		int t = znic_xfer(ZNIC_RX_POLL, 0, 0, hello_ok ? REPLY_CYC : PROBE_CYC);
		if (t < 0) {
			polls_unanswered++;
			return 0;
		}
		znic_dispatch();
	}
	if (!rx_data_pending)
		return 0;

	uint16_t n = rx_data_len;
	if (n > maxlen)
		n = maxlen;
	memcpy(buf, rx_data, n);
	rx_data_pending = 0;
	return n;
}

bool esp32link_send(const uint8_t *buf, uint16_t len)
{
	if (len > ZNIC_MAX_PAYLOAD)
		return false;
	int t = znic_xfer(ZNIC_DATA, buf, len, REPLY_CYC);
	if (t < 0) {
		printf("esp32link: TX not acked\n");
		return false;
	}
	if (t != ZNIC_DATA_ACK) {
		znic_dispatch();	/* unexpected but real: keep it */
		printf("esp32link: TX got type 0x%02x instead of DATA_ACK\n", t);
		return false;
	}
	return true;
}

void esp32link_debug_dump(void)
{
	printf("esp32link: hello=%d(#%d) fw=%u rst=%u sta_ack=%d status=%u link=%d "
		"rssi=%d phase=%d polls=%lu unanswered=%lu nops=%lu logs=%lu "
		"crc_err=%lu dropped=%lu ctl=0x%lx\n",
		hello_ok, hello_count, peer_fw, peer_rst, sta_acked, sta_status,
		link_up, last_rssi, phase, (unsigned long)polls_sent,
		(unsigned long)polls_unanswered, (unsigned long)nops_rx,
		(unsigned long)logs_rx, (unsigned long)crc_errors,
		(unsigned long)data_dropped, (unsigned long)reg_esp32_ctl);
}
