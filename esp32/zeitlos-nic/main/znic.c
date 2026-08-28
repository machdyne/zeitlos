#include "znic.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define ZNIC_UART   UART_NUM_1
#define ZNIC_TXD    17	/* ESP32 TX -> FPGA UART1_RX (N3) */
#define ZNIC_RXD    16	/* ESP32 RX <- FPGA UART1_TX (L1) */
#define ZNIC_BAUD   1000000
#define ZNIC_BUF    4096
#define ZNIC_LOG_MAX 180

/* znic_send() is called from the znic task, from app_main (HELLO) and
 * from whatever task happens to log (znic_log_vprintf); the mutex
 * keeps frames from interleaving on the wire. */
static SemaphoreHandle_t znic_mtx;

#define ZNIC_CTL_SLOTS 48
static QueueHandle_t ctlq;

void znic_ctl_init(void)
{
	ctlq = xQueueCreate(ZNIC_CTL_SLOTS, sizeof(znic_ctl_t));
}

int znic_ctl_push(uint8_t type, const uint8_t *payload, uint16_t n)
{
	if (!ctlq || n > ZNIC_CTL_MAX)
		return -1;
	znic_ctl_t m;
	m.type = type;
	m.len = n;
	if (n && payload)
		memcpy(m.payload, payload, n);
	return xQueueSend(ctlq, &m, 0) == pdTRUE ? 0 : -1;
}

int znic_ctl_pop(znic_ctl_t *out)
{
	if (!ctlq)
		return 0;
	return xQueueReceive(ctlq, out, 0) == pdTRUE ? 1 : 0;
}

uint16_t znic_crc16_update(uint16_t crc, const uint8_t *p, uint32_t n)
{
	while (n--) {
		crc ^= (uint16_t)(*p++) << 8;
		for (int i = 0; i < 8; i++)
			crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
			                     : (uint16_t)(crc << 1);
	}
	return crc;
}

uint16_t znic_crc16(const uint8_t *p, uint32_t n)
{
	return znic_crc16_update(0xFFFF, p, n);
}

void znic_init(void)
{
	uart_config_t cfg = {
		.baud_rate = ZNIC_BAUD,
		.data_bits = UART_DATA_8_BITS,
		.parity = UART_PARITY_DISABLE,
		.stop_bits = UART_STOP_BITS_1,
		.flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
		.source_clk = UART_SCLK_DEFAULT,
	};
	uart_driver_install(ZNIC_UART, ZNIC_BUF, ZNIC_BUF, 0, NULL, 0);
	uart_param_config(ZNIC_UART, &cfg);
	uart_set_pin(ZNIC_UART, ZNIC_TXD, ZNIC_RXD, UART_PIN_NO_CHANGE,
		UART_PIN_NO_CHANGE);
	znic_mtx = xSemaphoreCreateMutex();
}

/* No frame-sized stack buffers here: this runs on the znic task, on
 * app_main and on any logging task (some with small stacks). The CRC
 * is accumulated over the header and the payload in place. */
int znic_send(uint8_t type, const uint8_t *payload, uint16_t n)
{
	if (n > ZNIC_MAX_PAYLOAD)
		return -1;
	uint8_t hdr[6] = { ZNIC_SYNC0, ZNIC_SYNC1, ZNIC_VER, type,
		(uint8_t)(n & 0xFF), (uint8_t)(n >> 8) };
	uint16_t crc = znic_crc16_update(0xFFFF, hdr + 2, 4);
	if (n && payload)
		crc = znic_crc16_update(crc, payload, n);
	uint8_t crcb[2] = { (uint8_t)(crc & 0xFF), (uint8_t)(crc >> 8) };

	if (znic_mtx)
		xSemaphoreTake(znic_mtx, portMAX_DELAY);
	uart_write_bytes(ZNIC_UART, (const char *)hdr, 6);
	if (n && payload)
		uart_write_bytes(ZNIC_UART, (const char *)payload, n);
	uart_write_bytes(ZNIC_UART, (const char *)crcb, 2);
	if (znic_mtx)
		xSemaphoreGive(znic_mtx);
	return 0;
}

static int read_byte(uint8_t *c, int timeout_ms)
{
	int n = uart_read_bytes(ZNIC_UART, c, 1, pdMS_TO_TICKS(timeout_ms < 0 ? 0 : timeout_ms));
	return n == 1 ? 0 : -1;
}

int znic_recv(znic_msg_t *msg, int timeout_ms)
{
	int seen7e = 0;
	int waited = 0;
	const int slice = 20;
	while (waited <= timeout_ms || timeout_ms < 0) {
		uint8_t c;
		int t = slice;
		if (timeout_ms >= 0 && timeout_ms - waited < t)
			t = timeout_ms - waited;
		if (t < 1)
			t = 1;
		if (read_byte(&c, t) != 0) {
			waited += t;
			if (timeout_ms >= 0 && waited >= timeout_ms)
				return 0;
			continue;
		}
		if (!seen7e) {
			if (c == ZNIC_SYNC0)
				seen7e = 1;
			continue;
		}
		if (c != ZNIC_SYNC1) {
			seen7e = (c == ZNIC_SYNC0);
			continue;
		}
		uint8_t hdr[4];
		for (int i = 0; i < 4; i++) {
			if (read_byte(&hdr[i], 200) != 0)
				return -1;
		}
		if (hdr[0] != ZNIC_VER)
			return -1;
		uint16_t n = (uint16_t)hdr[2] | ((uint16_t)hdr[3] << 8);
		if (n > ZNIC_MAX_PAYLOAD)
			return -1;
		for (uint16_t i = 0; i < n; i++) {
			if (read_byte(&msg->payload[i], 200) != 0)
				return -1;
		}
		uint8_t crcl, crch;
		if (read_byte(&crcl, 200) != 0 || read_byte(&crch, 200) != 0)
			return -1;
		uint16_t want = znic_crc16_update(0xFFFF, hdr, 4);
		if (n)
			want = znic_crc16_update(want, msg->payload, n);
		uint16_t got = (uint16_t)crcl | ((uint16_t)crch << 8);
		if (want != got)
			return -1;
		msg->type = hdr[1];
		msg->len = n;
		return 1;
	}
	return 0;
}

/* ---- ESP_LOG mirror -------------------------------------------------
 * UART0 (K3/K4) is not reachable from the Zeitlos SOC bitstream, so
 * every log line is also framed as ZNIC_LOG on UART1; Zeitlos prints
 * it as "esp32: ...". ANSI colour escapes are stripped. Lines are
 * queued (never sent from here): only the znic task writes frames,
 * and only as replies. */
static vprintf_like_t prev_vprintf;
static volatile int quiet_uart0;

void znic_set_quiet_uart0(int quiet)
{
	quiet_uart0 = quiet;
}

unsigned znic_ctl_depth(void)
{
	return ctlq ? (unsigned)uxQueueMessagesWaiting(ctlq) : 0;
}

static int znic_log_vprintf(const char *fmt, va_list ap)
{
	va_list ap2;
	va_copy(ap2, ap);
	int r = 0;
	if (!quiet_uart0)
		r = prev_vprintf ? prev_vprintf(fmt, ap) : vprintf(fmt, ap);

	char raw[ZNIC_LOG_MAX + 1];
	int n = vsnprintf(raw, sizeof(raw), fmt, ap2);
	va_end(ap2);
	if (n <= 0)
		return r;
	if (n > ZNIC_LOG_MAX)
		n = ZNIC_LOG_MAX;

	char line[ZNIC_LOG_MAX + 1];
	int o = 0;
	for (int i = 0; i < n; i++) {
		if (raw[i] == '\033') {
			while (i < n && raw[i] != 'm')
				i++;
			continue;
		}
		if (raw[i] == '\r' || raw[i] == '\n')
			continue;
		line[o++] = raw[i];
	}
	if (o == 0)
		return r;
	if (xPortInIsrContext())
		return r;

	/* The WiFi library logs "I (123) wifi:" and the message text as two
	 * separate calls; glue them back into one line. */
	static char held[ZNIC_LOG_MAX + 1];
	static int held_len;
	if (o >= 5 && line[o - 1] == ':' && !memcmp(line + o - 5, "wifi:", 5)) {
		memcpy(held, line, o);
		held_len = o;
		return r;
	}
	if (held_len) {
		/* append into the static buffer: this hook runs on every
		 * logging task, some with little stack to spare */
		int n2 = held_len;
		if (n2 < ZNIC_LOG_MAX) held[n2++] = ' ';
		int copy = o;
		if (n2 + copy > ZNIC_LOG_MAX) copy = ZNIC_LOG_MAX - n2;
		memcpy(held + n2, line, copy);
		n2 += copy;
		held_len = 0;
		znic_ctl_push(ZNIC_LOG, (const uint8_t *)held, (uint16_t)n2);
		return r;
	}
	znic_ctl_push(ZNIC_LOG, (const uint8_t *)line, (uint16_t)o);	/* dropped if full */
	return r;
}

void znic_log_install(void)
{
	prev_vprintf = esp_log_set_vprintf(znic_log_vprintf);
}
