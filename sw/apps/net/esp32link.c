/*
 * Zeitlos -- UART PHY to the onboard ESP32 (ULX3S).
 *
 * Third NIC backend (NET_PHY=ESP32LINK). Speaks ZNIC over UART1;
 * 802.11 stays on the ESP32. See docs/esp32-net.md and znic.h.
 *
 * Every inbound message goes through ONE demux, znic_pump(). The
 * firmware answers RX_POLL with DATA or NOP, but HELLO, STA_ACK and
 * LINK arrive whenever the ESP32 feels like it -- i.e. into whichever
 * caller happens to be reading at that moment: esp32link_recv() (from
 * eth_poll) or esp32link_poll_wifi(). Before the demux, recv()
 * dropped STA_ACK on the floor and swallowed LINK without telling
 * the bring-up state machine (ULX3S 2026-08-27: "STA sent", then
 * silence).
 *
 * Firmware facts this relies on (esp32/zeitlos-nic/main/main.c):
 * HELLO is repeated for ~1.6 s after the ESP32 boots; STA is
 * answered by STA_ACK at once and then the ZNIC task blocks in the
 * association until it sends LINK (RX_POLLs queue up meanwhile and
 * are answered in a burst afterwards).
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
#define ACK_TIMEOUT     (2 * TICKS_PER_SEC)
#define STA_ACK_TIMEOUT (8 * TICKS_PER_SEC)
#define LINK_TIMEOUT    (40 * TICKS_PER_SEC)
#define HELLO_BURST     (3 * TICKS_PER_SEC)	/* firmware repeats HELLO
						   for ~1.6 s after boot;
						   later = it rebooted */
#define POLL_TIMEOUT    (TICKS_PER_SEC / 20)	/* ~50 ms */
#define BYTE_TIMEOUT    (TICKS_PER_SEC / 50)	/* ~20 ms between bytes
						   of one frame */

#define ESP32_CTL_EN    0x1
#define ESP32_CTL_GPIO0 0x2

/* last message pulled off the wire by znic_rx() */
static uint8_t rx_msg[ZNIC_MAX_PAYLOAD];
static uint16_t rx_msg_len;
static uint8_t rx_msg_type;

/* link state, all owned by znic_pump() */
static int hello_ok;
static int hello_count;
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
static uint32_t nops_rx;
static uint32_t logs_rx;
static uint8_t peer_rst;
static uint8_t peer_mac[6];

/* one-slot DATA queue: filled by znic_pump(), drained by
 * esp32link_recv(). The firmware only sends DATA in answer to an
 * RX_POLL, so one slot is enough as long as recv() drains before it
 * polls again. */
static uint8_t rx_data[ZNIC_MAX_PAYLOAD];
static uint16_t rx_data_len;
static int rx_data_pending;

/* wifi bring-up state machine, driven by esp32link_poll_wifi() */
enum { PH_HELLO = 0, PH_SEND_STA, PH_WAIT_LINK, PH_DONE };
static int phase;
static uint32_t sta_sent_tick;
static int link_timeout_reported;

static int uart1_rx_ready(void)
{
	return (reg_uart1_lsr & UART1_LSR_DR) != 0;
}

/* Data-ready is checked before the clock: bytes arrive every 10 us
 * at 1 Mbaud and the 16550 FIFO is 16 bytes deep, so the ready path
 * must be one MMIO read, not a z_uptime_ticks() call per byte. */
static int uart1_getc_timeout(uint8_t *c, uint32_t ticks)
{
	uint32_t start = 0;
	int timing = 0;
	for (;;) {
		if (uart1_rx_ready()) {
			*c = (uint8_t)reg_uart1_data;
			return 1;
		}
		if (!timing) {
			start = z_uptime_ticks();
			timing = 1;
			continue;
		}
		if (z_uptime_ticks() - start >= ticks)
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
	reg_uart1_fcr = 0x07;	/* FIFO on, flush, trigger 1 */
	reg_uart1_ier = 0x00;	/* poll, no IRQ */
}

static void znic_tx(uint8_t type, const uint8_t *payload, uint16_t n)
{
	uint8_t tmp[4 + ZNIC_MAX_PAYLOAD];
	tmp[0] = ZNIC_VER;
	tmp[1] = type;
	tmp[2] = (uint8_t)(n & 0xFF);
	tmp[3] = (uint8_t)(n >> 8);
	if (n && payload)
		memcpy(tmp + 4, payload, n);
	uint16_t crc = znic_crc16(tmp, 4 + n);

	uint32_t old = maskirq(0xFFFFFFFF);
	uart1_putc(ZNIC_SYNC0);
	uart1_putc(ZNIC_SYNC1);
	for (uint16_t i = 0; i < 4 + n; i++)
		uart1_putc(tmp[i]);
	uart1_putc((uint8_t)(crc & 0xFF));
	uart1_putc((uint8_t)(crc >> 8));
	maskirq(old);
}

/* Hunt one framed message. Returns 1 if rx_msg_* filled, 0 on timeout. */
static int znic_rx(uint32_t ticks)
{
	uint8_t c;
	uint32_t start = z_uptime_ticks();

	/* hunt 7E 5A */
	int seen7e = 0;
	for (;;) {
		if (!uart1_rx_ready()) {
			if (z_uptime_ticks() - start >= ticks)
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
	uint32_t bt = BYTE_TIMEOUT;
	if (bt < 8)
		bt = 8;
	for (int i = 0; i < 4; i++) {
		if (!uart1_getc_timeout(&hdr[i], bt))
			return 0;
	}
	uint16_t n = (uint16_t)hdr[2] | ((uint16_t)hdr[3] << 8);
	if (hdr[0] != ZNIC_VER || n > ZNIC_MAX_PAYLOAD) {
		crc_errors++;
		return 0;
	}
	for (uint16_t i = 0; i < n; i++) {
		if (!uart1_getc_timeout(&rx_msg[i], bt))
			return 0;
	}
	uint8_t crcl, crch;
	if (!uart1_getc_timeout(&crcl, bt) ||
			!uart1_getc_timeout(&crch, bt))
		return 0;
	uint16_t got = (uint16_t)crcl | ((uint16_t)crch << 8);

	uint16_t cacc = 0xFFFF;
	for (int i = 0; i < 4; i++) {
		cacc ^= (uint16_t)hdr[i] << 8;
		for (int b = 0; b < 8; b++)
			cacc = (cacc & 0x8000) ? (uint16_t)((cacc << 1) ^ 0x1021)
			                       : (uint16_t)(cacc << 1);
	}
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

static void znic_send_sta(const char *ssid, const char *psk)
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
	znic_tx(ZNIC_STA, body, (uint16_t)(2 + sl + pl));
	sta_acked = 0;
	sta_sent_tick = z_uptime_ticks();
	printf("esp32link: STA sent ssid='%s'\n", ssid);
}

/* The demux. Reads at most one message within `ticks` and applies it
 * to the state above. Returns the message type, or -1 if nothing
 * usable arrived (timeout, bad header, bad CRC). */
static int znic_pump(uint32_t ticks)
{
	if (!znic_rx(ticks))
		return -1;

	switch (rx_msg_type) {

	case ZNIC_HELLO:
		hello_count++;
		if (rx_msg_len >= 6)
			memcpy(peer_mac, rx_msg, 6);
		/* flags byte = esp_reset_reason(): 1 poweron, 3 sw, 4 panic,
		 * 5 int wdt, 6 task wdt, 9 brownout */
		peer_rst = (rx_msg_len >= 7) ? rx_msg[6] : 0;
		if (!hello_ok) {
			hello_ok = 1;
			first_hello_tick = z_uptime_ticks();
			printf("esp32link: HELLO fw=%u rst=%u mac %02x:%02x:%02x:%02x:%02x:%02x\n",
				(rx_msg_len >= 8) ? rx_msg[7] : 0, peer_rst,
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
			if (phase != PH_HELLO)
				phase = PH_SEND_STA;
		}
		/* else: the boot-time HELLO burst, already announced */
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

	case ZNIC_DATA:
		if (!rx_data_pending) {
			memcpy(rx_data, rx_msg, rx_msg_len);
			rx_data_len = rx_msg_len;
			rx_data_pending = 1;
		} else {
			data_dropped++;
		}
		znic_tx(ZNIC_DATA_ACK, 0, 0);
		break;

	case ZNIC_LOG:	/* one ESP_LOG line from the firmware */
		logs_rx++;
		printf("esp32: %.*s\n", (int)rx_msg_len, (const char *)rx_msg);
		break;

	case ZNIC_NOP:
		nops_rx++;
		break;

	default:	/* DATA_ACK, unknown */
		break;
	}

	return rx_msg_type;
}

bool esp32link_init(const uint8_t mac[6])
{
	(void)mac;
	hello_ok = 0;
	hello_count = 0;
	sta_acked = 0;
	sta_status = 0xff;
	link_up = 0;
	last_rssi = 0;
	last_reason = 0;
	last_scan = 0;
	crc_errors = 0;
	data_dropped = 0;
	polls_sent = 0;
	nops_rx = 0;
	logs_rx = 0;
	peer_rst = 0;
	rx_data_pending = 0;
	phase = PH_HELLO;
	link_timeout_reported = 0;

	uart1_init();

	/* gpio0=1, en=0 then en=1 so the ESP32 boots from flash */
	reg_esp32_ctl = ESP32_CTL_GPIO0;
	delay_ms(20);
	reg_esp32_ctl = ESP32_CTL_GPIO0 | ESP32_CTL_EN;

	/* Do not block here: a multi-second HELLO/STA wait freezes wm. */
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

	case PH_HELLO:
		znic_pump(POLL_TIMEOUT);
		if (hello_ok)
			phase = PH_SEND_STA;
		break;

	case PH_SEND_STA:
		znic_send_sta(ssid, psk);
		link_timeout_reported = 0;
		phase = PH_WAIT_LINK;
		break;

	case PH_WAIT_LINK:
		znic_pump(POLL_TIMEOUT);	/* STA_ACK / LINK / HELLO-again
						   land in the demux */
		if (phase != PH_WAIT_LINK)
			break;
		if (!sta_acked) {
			if (z_uptime_ticks() - sta_sent_tick > STA_ACK_TIMEOUT) {
				printf("esp32link: no STA_ACK in %us, resending STA\n",
					STA_ACK_TIMEOUT / TICKS_PER_SEC);
				phase = PH_SEND_STA;
			}
		} else if (!link_timeout_reported &&
				z_uptime_ticks() - sta_sent_tick > LINK_TIMEOUT) {
			printf("esp32link: no LINK %us after STA (still listening)\n",
				LINK_TIMEOUT / TICKS_PER_SEC);
			esp32link_debug_dump();
			link_timeout_reported = 1;
		}
		break;

	default:	/* PH_DONE: recv() keeps pumping LINK/HELLO events */
		break;
	}
}

/* Blocking variant (not used by net.c's ESP32LINK build, kept for the
 * phy_wifi_sta contract). */
bool esp32link_wifi_sta(const char *ssid, const char *psk)
{
	znic_send_sta(ssid, psk);
	uint32_t start = z_uptime_ticks();
	while (!sta_acked && z_uptime_ticks() - start < STA_ACK_TIMEOUT)
		znic_pump(POLL_TIMEOUT);
	if (!sta_acked) {
		printf("esp32link: no STA_ACK\n");
		return false;
	}
	if (sta_status != ZNIC_STA_OK)
		return false;
	printf("esp32link: waiting for LINK...\n");
	phase = PH_WAIT_LINK;
	start = z_uptime_ticks();
	while (phase == PH_WAIT_LINK && z_uptime_ticks() - start < LINK_TIMEOUT)
		znic_pump(POLL_TIMEOUT);
	if (phase == PH_WAIT_LINK) {
		printf("esp32link: no LINK (association timed out)\n");
		return false;
	}
	return link_up != 0;
}

uint16_t esp32link_recv(uint8_t *buf, uint16_t maxlen)
{
	/* drain what is already queued (the burst after the firmware was
	 * busy associating) before asking for more */
	while (!rx_data_pending && uart1_rx_ready()) {
		if (znic_pump(BYTE_TIMEOUT) < 0)
			break;
	}
	if (!rx_data_pending) {
		znic_tx(ZNIC_RX_POLL, 0, 0);
		polls_sent++;
		znic_pump(POLL_TIMEOUT);
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
	znic_tx(ZNIC_DATA, buf, len);
	uint32_t start = z_uptime_ticks();
	while (z_uptime_ticks() - start < ACK_TIMEOUT) {
		uint32_t left = ACK_TIMEOUT - (z_uptime_ticks() - start);
		if (left < 8)
			left = 8;
		if (znic_pump(left) == ZNIC_DATA_ACK)
			return true;
	}
	printf("esp32link: TX not acked\n");
	return false;
}

void esp32link_debug_dump(void)
{
	printf("esp32link: hello=%d(#%d) rst=%u sta_ack=%d status=%u link=%d rssi=%d "
		"phase=%d polls=%lu nops=%lu logs=%lu crc_err=%lu dropped=%lu ctl=0x%lx\n",
		hello_ok, hello_count, peer_rst, sta_acked, sta_status, link_up,
		last_rssi, phase, (unsigned long)polls_sent, (unsigned long)nops_rx,
		(unsigned long)logs_rx, (unsigned long)crc_errors,
		(unsigned long)data_dropped, (unsigned long)reg_esp32_ctl);
}
