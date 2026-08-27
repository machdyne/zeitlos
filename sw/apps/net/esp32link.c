/*
 * Zeitlos -- UART PHY to the onboard ESP32 (ULX3S).
 *
 * Third NIC backend (NET_PHY=ESP32LINK). Speaks ZNIC over UART1;
 * 802.11 stays on the ESP32. See docs/esp32-net.md and znic.h.
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
#define HELLO_TIMEOUT   (5 * TICKS_PER_SEC)
#define ACK_TIMEOUT     (2 * TICKS_PER_SEC)
#define LINK_TIMEOUT    (40 * TICKS_PER_SEC)
#define POLL_TIMEOUT    (TICKS_PER_SEC / 20)	/* ~50 ms */
#define BYTE_TIMEOUT    (TICKS_PER_SEC / 50)	/* ~20 ms; do not use the
						   whole HELLO/LINK budget
						   per missing byte */

#define ESP32_CTL_EN    0x1
#define ESP32_CTL_GPIO0 0x2

static uint8_t rx_msg[ZNIC_MAX_PAYLOAD];
static uint16_t rx_msg_len;
static uint8_t rx_msg_type;
static int hello_ok;
static int link_up;
static int last_rssi;
static uint32_t crc_errors;
static uint8_t peer_mac[6];

static int uart1_rx_ready(void)
{
	return (reg_uart1_lsr & UART1_LSR_DR) != 0;
}

static int uart1_getc_timeout(uint8_t *c, uint32_t ticks)
{
	uint32_t start = z_uptime_ticks();
	while (!uart1_rx_ready()) {
		if (z_uptime_ticks() - start >= ticks)
			return 0;
	}
	*c = (uint8_t)reg_uart1_data;
	return 1;
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
	while (z_uptime_ticks() - start < ticks) {
		if (!uart1_rx_ready())
			continue;
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
		goto have_sync;
	}
	return 0;

have_sync:
	{
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
}

static int wait_type(uint8_t want, uint32_t ticks)
{
	uint32_t start = z_uptime_ticks();
	while (z_uptime_ticks() - start < ticks) {
		uint32_t left = ticks - (z_uptime_ticks() - start);
		if (left < 8)
			left = 8;
		if (!znic_rx(left))
			continue;
		if (rx_msg_type == want)
			return 1;
		if (rx_msg_type == ZNIC_LINK && rx_msg_len >= 1) {
			link_up = rx_msg[0] ? 1 : 0;
			if (rx_msg_len >= 2)
				last_rssi = (int8_t)rx_msg[1];
		}
	}
	return 0;
}

bool esp32link_init(const uint8_t mac[6])
{
	(void)mac;
	hello_ok = 0;
	link_up = 0;
	crc_errors = 0;

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
	static int phase;	/* 0 hello, 1 send STA, 2 wait LINK, 3 done */

	if (phase >= 3 || !ssid || !ssid[0])
		return;

	if (phase == 0) {
		if (!znic_rx(POLL_TIMEOUT))
			return;
		if (rx_msg_type != ZNIC_HELLO)
			return;
		hello_ok = 1;
		if (rx_msg_len >= 6)
			memcpy(peer_mac, rx_msg, 6);
		printf("esp32link: HELLO fw=%u mac %02x:%02x:%02x:%02x:%02x:%02x\n",
			(rx_msg_len >= 8) ? rx_msg[7] : 0,
			peer_mac[0], peer_mac[1], peer_mac[2],
			peer_mac[3], peer_mac[4], peer_mac[5]);
		phase = 1;
		return;
	}

	if (phase == 1) {
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
		printf("esp32link: STA sent ssid='%s'\n", ssid);
		phase = 2;
		return;
	}

	if (!znic_rx(POLL_TIMEOUT))
		return;
	if (rx_msg_type == ZNIC_STA_ACK) {
		printf("esp32link: STA_ACK status %u\n",
			(rx_msg_len >= 1) ? rx_msg[0] : 0xff);
		return;
	}
	if (rx_msg_type == ZNIC_LINK && rx_msg_len >= 1) {
		link_up = rx_msg[0] ? 1 : 0;
		if (rx_msg_len >= 2)
			last_rssi = (int8_t)rx_msg[1];
		if (link_up)
			printf("esp32link: LINK up rssi=%d\n", last_rssi);
		else
			printf("esp32link: LINK down reason=%u scan=%u\n",
				(rx_msg_len >= 3) ? rx_msg[2] : 0,
				(rx_msg_len >= 4) ? rx_msg[3] : 0);
		phase = 3;
	}
}

bool esp32link_wifi_sta(const char *ssid, const char *psk)
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
	if (!wait_type(ZNIC_STA_ACK, ACK_TIMEOUT * 4)) {
		printf("esp32link: no STA_ACK\n");
		return false;
	}
	if (rx_msg_len < 1 || rx_msg[0] != ZNIC_STA_OK) {
		printf("esp32link: STA_ACK status %u\n",
			(rx_msg_len >= 1) ? rx_msg[0] : 0xff);
		return false;
	}
	printf("esp32link: waiting for LINK...\n");
	if (!wait_type(ZNIC_LINK, LINK_TIMEOUT)) {
		printf("esp32link: no LINK (association timed out)\n");
		return false;
	}
	if (rx_msg_len >= 1)
		link_up = rx_msg[0] ? 1 : 0;
	if (rx_msg_len >= 2)
		last_rssi = (int8_t)rx_msg[1];
	if (!link_up) {
		unsigned reason = (rx_msg_len >= 3) ? rx_msg[2] : 0;
		unsigned scan = (rx_msg_len >= 4) ? rx_msg[3] : 0;
		printf("esp32link: LINK down reason=%u scan=%u\n", reason, scan);
		return false;
	}
	printf("esp32link: LINK up rssi=%d\n", last_rssi);
	return true;
}

uint16_t esp32link_recv(uint8_t *buf, uint16_t maxlen)
{
	znic_tx(ZNIC_RX_POLL, 0, 0);
	if (!znic_rx(POLL_TIMEOUT))
		return 0;
	if (rx_msg_type == ZNIC_NOP)
		return 0;
	if (rx_msg_type == ZNIC_LINK && rx_msg_len >= 1) {
		link_up = rx_msg[0] ? 1 : 0;
		return 0;
	}
	if (rx_msg_type != ZNIC_DATA)
		return 0;
	uint16_t n = rx_msg_len;
	if (n > maxlen)
		n = maxlen;
	memcpy(buf, rx_msg, n);
	znic_tx(ZNIC_DATA_ACK, 0, 0);
	return n;
}

bool esp32link_send(const uint8_t *buf, uint16_t len)
{
	if (len > ZNIC_MAX_PAYLOAD)
		return false;
	znic_tx(ZNIC_DATA, buf, len);
	if (!wait_type(ZNIC_DATA_ACK, ACK_TIMEOUT)) {
		printf("esp32link: TX not acked\n");
		return false;
	}
	return true;
}

void esp32link_debug_dump(void)
{
	printf("esp32link: hello=%d link=%d rssi=%d crc_err=%lu ctl=0x%lx\n",
		hello_ok, link_up, last_rssi, (unsigned long)crc_errors,
		(unsigned long)reg_esp32_ctl);
}
