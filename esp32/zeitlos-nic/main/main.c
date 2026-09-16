/*
 * zeitlos-nic -- ESP32 firmware for ULX3S.
 *
 * UART1 (GPIO16/17) speaks ZNIC with Zeitlos. UART0 is log / esptool.
 * Never init SDMMC or GPIO 2/4/12-15.
 */

#include "znic.h"
#include "gateway.h"
#include "selftest.h"
#include "tftpd.h"
#include "screend.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>

static const char *TAG = "nic";

/* HELLO and LINK are queued, not sent: Zeitlos collects them with
 * RX_POLL (see znic.h). Only replies are written directly. */
static void queue_hello(void)
{
	uint8_t body[8];
	memset(body, 0, sizeof(body));
	esp_read_mac(body, ESP_MAC_WIFI_STA);
	body[0] |= 0x02;
	body[6] = (uint8_t)esp_reset_reason();	/* flags: why we booted */
	body[7] = 2;	/* fw ver: 2 = solicited protocol */
	znic_ctl_push(ZNIC_HELLO, body, 8);
}

static void queue_link(int up, int rssi)
{
	/* bytes 4-7: the station address, big-endian, zero until there is
	 * one -- Zeitlos prints it so a headless board tells you where to
	 * reach its gateway on the LAN */
	uint32_t ip = up ? gateway_sta_ip() : 0;
	uint8_t body[8] = {
		(uint8_t)(up ? 1 : 0),
		(uint8_t)rssi,
		(uint8_t)gateway_last_disc_reason(),
		(uint8_t)gateway_last_scan_found(),
		(uint8_t)(ip >> 24), (uint8_t)(ip >> 16),
		(uint8_t)(ip >> 8), (uint8_t)ip,
	};
	znic_ctl_push(ZNIC_LINK, body, 8);
}

/* The association runs on its own task so the znic task keeps
 * answering RX_POLL (Zeitlos waits for every reply with interrupts
 * masked; an unanswered poll costs it a few ms). */
#define STA_LIST_MAX 4
static char sta_ssid[STA_LIST_MAX][33];
static char sta_psk[STA_LIST_MAX][65];
static int  sta_count;
static volatile int wifi_busy;

static void wifi_task(void *arg)
{
	(void)arg;
	int rssi = 0;
	int ok = -1;
	/* in NET.CFG order; gateway_wifi_sta() scans first and fails fast
	 * (~3s, reason 201) when a network is not there, so absent entries
	 * are cheap to skip */
	for (int i = 0; i < sta_count && ok != 0; i++) {
		ESP_LOGI(TAG, "trying network %d/%d '%s'", i + 1, sta_count,
			sta_ssid[i]);
		ok = gateway_wifi_sta(sta_ssid[i], sta_psk[i], &rssi);
	}
	if (ok == 0)
		queue_link(1, rssi);
	else
		queue_link(0, 0);
	wifi_busy = 0;
	vTaskDelete(NULL);
}

/* same path the ZNIC STA message takes, callable from the selftest CLI */
int nic_start_sta(const char *ssid, const char *psk)
{
	if (wifi_busy)
		return -1;
	memset(sta_ssid, 0, sizeof(sta_ssid));
	memset(sta_psk, 0, sizeof(sta_psk));
	strncpy(sta_ssid[0], ssid, sizeof(sta_ssid[0]) - 1);
	strncpy(sta_psk[0], psk ? psk : "", sizeof(sta_psk[0]) - 1);
	sta_count = 1;
	wifi_busy = 1;
	gateway_wait_ready();
	xTaskCreate(wifi_task, "wifi", 12288, NULL, 4, NULL);
	return 0;
}

static void znic_task(void *arg)
{
	(void)arg;
	/* static: 1.5 KiB frames do not belong on this task's stack --
	 * the STA/scan path (gateway_wifi_sta, wifi_ap_record_t[]) runs
	 * here too and overflowed 8 KiB under Zeitlos (2026-08-27). */
	static znic_msg_t msg;
	static uint8_t frame[ZNIC_MAX_PAYLOAD];
	static znic_ctl_t ctl;
	static uint32_t polls;
	static int quiet;
	TickType_t last_stat = xTaskGetTickCount();
	for (;;) {
		int r = znic_recv(&msg, 100);
		/* one status line a minute so Zeitlos can see this side */
		if (xTaskGetTickCount() - last_stat > pdMS_TO_TICKS(60000)) {
			last_stat = xTaskGetTickCount();
			uint32_t fz, tz, dz;
			gateway_stats(&fz, &tz, &dz);
			ESP_LOGI(TAG, "stat up=%lus polls=%lu fromZ=%lu toZ=%lu drop=%lu ctlq=%u ip=%d",
				(unsigned long)(xTaskGetTickCount() / configTICK_RATE_HZ),
				(unsigned long)polls, (unsigned long)fz, (unsigned long)tz,
				(unsigned long)dz, znic_ctl_depth(), gateway_sta_got_ip());
		}
		if (r <= 0)
			continue;
		switch (msg.type) {
		case ZNIC_RX_POLL: {
			uint16_t n = 0;
			polls++;
			if (!quiet) {
				quiet = 1;
				znic_set_quiet_uart0(1);
			}
			/* znic v3: the poll may carry a credit -- how many frames
			 * the FPGA's receive FIFO can take right now. With >= 2,
			 * everything ready goes out in one BURST instead of one
			 * frame per round trip. The queue snapshot cannot come up
			 * short: this task is the only consumer. */
			uint8_t credit = (msg.len >= 1 && msg.payload[0]) ? msg.payload[0] : 1;
			if (credit > 15)
				credit = 15;
			int avail = znic_ctl_depth() + gateway_pending_to_zeitlos();
			int burst = avail < credit ? avail : credit;
			if (burst >= 2) {
				uint8_t bn = (uint8_t)burst;
				znic_send(ZNIC_BURST, &bn, 1);
				for (int i = 0; i < burst; i++) {
					if (znic_ctl_pop(&ctl))
						znic_send(ctl.type, ctl.payload, ctl.len);
					else if (gateway_pop_to_zeitlos(frame, &n) && n)
						znic_send(ZNIC_DATA, frame, n);
					else
						znic_send(ZNIC_NOP, NULL, 0);
				}
			} else if (znic_ctl_pop(&ctl))
				znic_send(ctl.type, ctl.payload, ctl.len);
			else if (gateway_pop_to_zeitlos(frame, &n) && n)
				znic_send(ZNIC_DATA, frame, n);
			else
				znic_send(ZNIC_NOP, NULL, 0);
			break;
		}
		case ZNIC_DATA:
			gateway_from_zeitlos(msg.payload, msg.len);
			znic_send(ZNIC_DATA_ACK, NULL, 0);
			break;
		case ZNIC_STA: {
			if (msg.len < 2) {
				uint8_t st = 1;
				znic_send(ZNIC_STA_ACK, &st, 1);
				break;
			}
			/* one or more (ssid_len, ssid, psk_len, psk) tuples, in
			 * order of preference -- wifi_task tries them first to
			 * last and keeps the first that exists */
			char ssids[STA_LIST_MAX][33], psks[STA_LIST_MAX][65];
			memset(ssids, 0, sizeof(ssids));
			memset(psks, 0, sizeof(psks));
			int n = 0;
			uint16_t off = 0;
			while (off + 2 <= msg.len && n < STA_LIST_MAX) {
				uint8_t sl = msg.payload[off];
				if (off + 1 + sl + 1 > msg.len || sl > 32)
					break;
				uint8_t pl = msg.payload[off + 1 + sl];
				if (off + 2 + sl + pl > msg.len || pl > 64)
					break;
				memcpy(ssids[n], msg.payload + off + 1, sl);
				memcpy(psks[n], msg.payload + off + 2 + sl, pl);
				off += 2 + sl + pl;
				n++;
			}
			if (n == 0) {
				uint8_t st = 1;
				znic_send(ZNIC_STA_ACK, &st, 1);
				break;
			}
			ESP_LOGI(TAG, "STA: %d network(s), first '%s'", n, ssids[0]);
			uint8_t st = 0;
			znic_send(ZNIC_STA_ACK, &st, 1);
			if (wifi_busy) {
				ESP_LOGW(TAG, "STA while associating: ignored");
				break;
			}
			memcpy(sta_ssid, ssids, sizeof(sta_ssid));
			memcpy(sta_psk, psks, sizeof(sta_psk));
			sta_count = n;
			wifi_busy = 1;
			gateway_wait_ready();
			xTaskCreate(wifi_task, "wifi", 10240, NULL, 4, NULL);
			break;
		}
		case ZNIC_NOP:
		case ZNIC_DATA_ACK:
			break;
		default:
			break;
		}
	}
}

/* ULX3S microSD is on ESP32 GPIO 2/4/12-15. Keep them as inputs so
 * the card stays usable after wifi_en goes high. */
static void sd_pins_hiz(void)
{
	const int pins[] = { 2, 4, 12, 13, 14, 15 };
	for (int i = 0; i < 6; i++) {
		gpio_config_t io = {
			.pin_bit_mask = 1ULL << pins[i],
			.mode = GPIO_MODE_INPUT,
			.pull_up_en = GPIO_PULLUP_DISABLE,
			.pull_down_en = GPIO_PULLDOWN_DISABLE,
			.intr_type = GPIO_INTR_DISABLE,
		};
		gpio_config(&io);
	}
}

void app_main(void)
{
	sd_pins_hiz();
	ESP_LOGI(TAG, "zeitlos-nic starting (UART1 1Mbaud GPIO16/17)");
	znic_init();
	znic_ctl_init();
	queue_hello();		/* first thing Zeitlos's first RX_POLL gets */
	znic_log_install();	/* ESP_LOG lines reach Zeitlos as ZNIC_LOG */
	xTaskCreate(znic_task, "znic", 16384, NULL, 19, NULL);	/* above tcpip (18): answer polls promptly */
	gateway_init();
	tftpd_start();
	screend_start();
	selftest_start();
}
