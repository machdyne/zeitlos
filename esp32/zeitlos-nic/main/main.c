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
	uint8_t body[4] = {
		(uint8_t)(up ? 1 : 0),
		(uint8_t)rssi,
		(uint8_t)gateway_last_disc_reason(),
		(uint8_t)gateway_last_scan_found(),
	};
	znic_ctl_push(ZNIC_LINK, body, 4);
}

/* The association runs on its own task so the znic task keeps
 * answering RX_POLL (Zeitlos waits for every reply with interrupts
 * masked; an unanswered poll costs it a few ms). */
static char sta_ssid[33];
static char sta_psk[65];
static volatile int wifi_busy;

static void wifi_task(void *arg)
{
	(void)arg;
	int rssi = 0;
	if (gateway_wifi_sta(sta_ssid, sta_psk, &rssi) == 0)
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
	strncpy(sta_ssid, ssid, sizeof(sta_ssid) - 1);
	strncpy(sta_psk, psk ? psk : "", sizeof(sta_psk) - 1);
	wifi_busy = 1;
	gateway_wait_ready();
	xTaskCreate(wifi_task, "wifi", 10240, NULL, 4, NULL);
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
		/* one status line every 10 s so Zeitlos can see this side */
		if (xTaskGetTickCount() - last_stat > pdMS_TO_TICKS(10000)) {
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
			if (znic_ctl_pop(&ctl))
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
			uint8_t sl = msg.payload[0];
			if (1 + sl + 1 > msg.len) {
				uint8_t st = 1;
				znic_send(ZNIC_STA_ACK, &st, 1);
				break;
			}
			uint8_t pl = msg.payload[1 + sl];
			char ssid[33], psk[65];
			memset(ssid, 0, sizeof(ssid));
			memset(psk, 0, sizeof(psk));
			if (sl > 32)
				sl = 32;
			if (pl > 63)
				pl = 63;
			memcpy(ssid, msg.payload + 1, sl);
			memcpy(psk, msg.payload + 2 + sl, pl);
			ESP_LOGI(TAG, "STA ssid='%s'", ssid);
			uint8_t st = 0;
			znic_send(ZNIC_STA_ACK, &st, 1);
			if (wifi_busy) {
				ESP_LOGW(TAG, "STA while associating: ignored");
				break;
			}
			memcpy(sta_ssid, ssid, sizeof(sta_ssid));
			memcpy(sta_psk, psk, sizeof(sta_psk));
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
	selftest_start();
}
