/*
 * Dual-netif gateway: znic (192.168.4.1/24, Ethernet over UART) + STA.
 * NAPT on the STA address so Zeitlos (typically 192.168.4.2) can reach
 * the AP's LAN / the internet. See docs/esp32-net.md.
 *
 * Do not touch GPIO 2/4/12/13/14/15 (ULX3S microSD).
 */

#include "gateway.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/ip4_addr.h"
#include "lwip/ip.h"
#include "lwip/def.h"
#include "netif/ethernet.h"
#include <string.h>
#include "lwip/tcpip.h"

#ifdef CONFIG_LWIP_IPV4_NAPT
#if defined(__has_include)
#if __has_include("lwip/lwip_napt.h")
#include "lwip/lwip_napt.h"
#elif __has_include("lwip/ip4_napt.h")
#include "lwip/ip4_napt.h"
#endif
#endif
#endif

static const char *TAG = "gw";

#define ZNIC_IP   "192.168.4.1"
#define ZNIC_MASK "255.255.255.0"
#define RXQ_DEPTH 4
#define FRAME_MAX 1518

static struct netif znic_nif;
static uint8_t znic_mac[6];
static uint8_t rxq[RXQ_DEPTH][FRAME_MAX];
static uint16_t rxq_len[RXQ_DEPTH];
static volatile int rxq_head, rxq_tail;
static volatile int sta_got_ip;
static volatile int gw_ready;
static uint32_t sta_ip_u32;
static int last_rssi;
static volatile int last_disc_reason;
static volatile int last_scan_found;
static volatile int sta_retry;
static int wifi_started;

static uint32_t n_to_z, n_from_z, n_drop_z;

static int rxq_push(const uint8_t *f, uint16_t n)
{
	int next = (rxq_head + 1) % RXQ_DEPTH;
	if (next == rxq_tail)
		return -1;
	if (n > FRAME_MAX)
		n = FRAME_MAX;
	memcpy(rxq[rxq_head], f, n);
	rxq_len[rxq_head] = n;
	rxq_head = next;
	return 0;
}

void gateway_stats(uint32_t *from_z, uint32_t *to_z, uint32_t *dropped)
{
	*from_z = n_from_z;
	*to_z = n_to_z;
	*dropped = n_drop_z;
}

void gateway_znic_mac(uint8_t out[6])
{
	memcpy(out, znic_mac, 6);
}

int gateway_pop_to_zeitlos(uint8_t *out, uint16_t *len)
{
	if (rxq_tail == rxq_head)
		return 0;
	uint16_t n = rxq_len[rxq_tail];
	memcpy(out, rxq[rxq_tail], n);
	*len = n;
	rxq_tail = (rxq_tail + 1) % RXQ_DEPTH;
	return 1;
}

static err_t znic_linkoutput(struct netif *nif, struct pbuf *p)
{
	(void)nif;
	uint8_t buf[FRAME_MAX];
	u16_t n = pbuf_copy_partial(p, buf, sizeof(buf), 0);
	/* tcpip thread: no logging here (its stack is small; the log hook
	 * alone needs ~1 KiB). Counted, reported when popped. */
	if (rxq_push(buf, n) != 0)
		n_drop_z++;
	n_to_z++;
	return ERR_OK;
}

static err_t znic_output(struct netif *nif, struct pbuf *p, const ip4_addr_t *ipaddr)
{
	return etharp_output(nif, p, ipaddr);
}

static err_t znic_if_init(struct netif *nif)
{
	nif->name[0] = 'z';
	nif->name[1] = 'n';
	nif->output = znic_output;
	nif->linkoutput = znic_linkoutput;
	nif->mtu = 1500;
	nif->hwaddr_len = 6;
	memcpy(nif->hwaddr, znic_mac, 6);
	nif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP
		| NETIF_FLAG_UP;
	return ERR_OK;
}

/* Called on the znic task. The netif's input is tcpip_input, which
 * hands the pbuf to the lwIP thread; calling ethernet_input here
 * directly (as before) bypassed the core lock -- ARP replies still
 * came back, forwarding + NAPT did not. */
int gateway_from_zeitlos(const uint8_t *frame, uint16_t len)
{
	if (!gw_ready || len < 14)
		return -1;
	struct pbuf *p = pbuf_alloc(PBUF_RAW, len, PBUF_RAM);
	if (!p)
		return -1;
	pbuf_take(p, frame, len);
	n_from_z++;
	if (znic_nif.input(p, &znic_nif) != ERR_OK) {
		ESP_LOGW(TAG, "Z->wifi: input rejected");
		pbuf_free(p);
	}
	return 0;
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
	(void)arg;
	(void)base;
	if (id == WIFI_EVENT_STA_DISCONNECTED) {
		wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
		last_disc_reason = d ? d->reason : 0;
		sta_got_ip = 0;
		ESP_LOGW(TAG, "STA disconnect reason=%d", last_disc_reason);
		/* 2=AUTH_FAIL, 15=4WAY timeout, 201=NO_AP: don't hammer */
		if (last_disc_reason == 2 || last_disc_reason == 15 ||
				last_disc_reason == 201)
			return;
		if (sta_retry++ < 8)
			esp_wifi_connect();
	}
}

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
	(void)arg;
	(void)base;
	if (id != IP_EVENT_STA_GOT_IP)
		return;
	ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
	sta_ip_u32 = ntohl(e->ip_info.ip.addr);
	sta_got_ip = 1;
	ESP_LOGI(TAG, "STA got IP " IPSTR, IP2STR(&e->ip_info.ip));
}

int gateway_init(void)
{
	esp_err_t e = nvs_flash_init();
	if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		nvs_flash_erase();
		nvs_flash_init();
	}
	esp_netif_init();
	esp_event_loop_create_default();
	esp_netif_create_default_wifi_sta();

	wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
	esp_wifi_init(&wcfg);
	esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi, NULL);
	esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_ip, NULL);
	esp_wifi_set_mode(WIFI_MODE_STA);
	wifi_country_t country = {
		.cc = "ES", .schan = 1, .nchan = 13,
		.max_tx_power = 20, .policy = WIFI_COUNTRY_POLICY_AUTO,
	};
	esp_wifi_set_country(&country);
	/* Radio stays off until STA/scan. Starting it at boot + 20 dBm
	 * TX brownouts the PicoRV32 (BIOS ZB / logo loop). */

	esp_read_mac(znic_mac, ESP_MAC_WIFI_STA);
	znic_mac[0] |= 0x02;	/* locally administered, distinct from STA */

	ip4_addr_t ip, mask, gw;
	ip4addr_aton(ZNIC_IP, &ip);
	ip4addr_aton(ZNIC_MASK, &mask);
	ip4_addr_set_zero(&gw);
	LOCK_TCPIP_CORE();
	netif_add(&znic_nif, &ip, &mask, &gw, NULL, znic_if_init, tcpip_input);
	netif_set_up(&znic_nif);
	netif_set_link_up(&znic_nif);
	UNLOCK_TCPIP_CORE();
	gw_ready = 1;
	return 0;
}

void gateway_wait_ready(void)
{
	while (!gw_ready)
		vTaskDelay(pdMS_TO_TICKS(50));
}

int gateway_last_disc_reason(void)
{
	return last_disc_reason;
}

int gateway_last_scan_found(void)
{
	return last_scan_found;
}

int gateway_sta_got_ip(void)
{
	return sta_got_ip;
}

uint32_t gateway_sta_ip(void)
{
	return sta_ip_u32;
}

static void wifi_radio_on(void)
{
	if (wifi_started)
		return;
	esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
	esp_wifi_start();
	/* unit is 0.25 dBm; 8 = 2 dBm (IDF minimum), 78 = 19.5 dBm (max).
	 * 2026-08-27: 8 while the ESP32 reset loops were being chased
	 * (they were stack overflows, not power); ZNIC_TX_POWER raises it
	 * to see whether the rail really copes. */
#ifndef ZNIC_TX_POWER
#define ZNIC_TX_POWER 78
#endif
	esp_wifi_set_max_tx_power(ZNIC_TX_POWER);
	int8_t got = 0;
	esp_wifi_get_max_tx_power(&got);
	ESP_LOGI(TAG, "tx power set %d -> %d (x0.25 dBm)", ZNIC_TX_POWER, got);
	vTaskDelay(pdMS_TO_TICKS(400));
	wifi_started = 1;
}

int gateway_wifi_scan(const char *want_ssid, gateway_ap_t *out, int maxn)
{
	gateway_wait_ready();
	wifi_radio_on();
	last_scan_found = 0;

	wifi_scan_config_t sc = { 0 };
	sc.show_hidden = true;
	esp_wifi_scan_start(&sc, true);
	uint16_t ap_num = 0;
	esp_wifi_scan_get_ap_num(&ap_num);
	uint16_t n = ap_num > GATEWAY_SCAN_MAX ? GATEWAY_SCAN_MAX : ap_num;
	wifi_ap_record_t recs[GATEWAY_SCAN_MAX];
	if (n)
		esp_wifi_scan_get_ap_records(&n, recs);

	int copy = n;
	if (maxn >= 0 && copy > maxn)
		copy = maxn;
	for (int i = 0; i < (int)n; i++) {
		ESP_LOGI(TAG, "AP '%s' rssi=%d auth=%d ch=%d",
			recs[i].ssid, recs[i].rssi, recs[i].authmode, recs[i].primary);
		if (want_ssid && want_ssid[0] &&
				strcmp((char *)recs[i].ssid, want_ssid) == 0)
			last_scan_found = 1;
		if (out && i < copy) {
			memset(&out[i], 0, sizeof(out[i]));
			strncpy(out[i].ssid, (char *)recs[i].ssid, 32);
			out[i].rssi = recs[i].rssi;
			out[i].ch = recs[i].primary;
			out[i].auth = recs[i].authmode;
		}
	}
	ESP_LOGI(TAG, "scan %u APs (of %u), ssid='%s' found=%d",
		n, ap_num, want_ssid ? want_ssid : "", last_scan_found);
	return (int)n;
}

int gateway_wifi_sta(const char *ssid, const char *psk, int *rssi)
{
	gateway_wait_ready();
	last_disc_reason = 0;
	sta_retry = 0;
	sta_got_ip = 0;

	esp_wifi_disconnect();
	vTaskDelay(pdMS_TO_TICKS(150));

	gateway_wifi_scan(ssid, NULL, 0);
	if (ssid && ssid[0] && !last_scan_found) {
		last_disc_reason = 201;
		return -1;
	}

	wifi_config_t cfg = { 0 };
	strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
	if (psk && psk[0])
		strncpy((char *)cfg.sta.password, psk, sizeof(cfg.sta.password) - 1);
	cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
	cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
	cfg.sta.pmf_cfg.capable = true;
	cfg.sta.pmf_cfg.required = false;
	cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;

	esp_err_t e = esp_wifi_set_config(WIFI_IF_STA, &cfg);
	if (e != ESP_OK) {
		ESP_LOGE(TAG, "set_config %s", esp_err_to_name(e));
		return -1;
	}
	e = esp_wifi_connect();
	if (e != ESP_OK) {
		ESP_LOGE(TAG, "connect %s", esp_err_to_name(e));
		return -1;
	}

	for (int i = 0; i < 250; i++) {
		if (sta_got_ip)
			break;
		if (last_disc_reason == 2 || last_disc_reason == 15)
			break;
		vTaskDelay(pdMS_TO_TICKS(100));
	}
	if (!sta_got_ip) {
		ESP_LOGE(TAG, "STA no IP (ssid='%s' reason=%d)", ssid, last_disc_reason);
		return -1;
	}

	wifi_ap_record_t ap;
	if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
		last_rssi = ap.rssi;
	if (rssi)
		*rssi = last_rssi;
#ifdef CONFIG_LWIP_IPV4_NAPT
	/* esp-lwip's `napt` flag marks the INSIDE netif (the one whose
	 * clients get masqueraded -- the softAP in IDF's router example):
	 * ip4_forward() translates when the *output* netif has napt==0
	 * and ip4_input() untranslates when the *input* netif has
	 * napt==0. Enabling it on the STA (as before) made 192.168.4.2
	 * leave untranslated. Not in the sys_evt handler: the table init
	 * overflows that stack. */
	LOCK_TCPIP_CORE();
	int napt_ok = ip_napt_enable_netif(&znic_nif, 1);
	UNLOCK_TCPIP_CORE();
	ESP_LOGI(TAG, "NAPT on znic (inside) -> STA %lu.%lu.%lu.%lu: %s",
		(unsigned long)((sta_ip_u32 >> 24) & 0xff),
		(unsigned long)((sta_ip_u32 >> 16) & 0xff),
		(unsigned long)((sta_ip_u32 >> 8) & 0xff),
		(unsigned long)(sta_ip_u32 & 0xff), napt_ok ? "ok" : "FAILED");
#else
	ESP_LOGW(TAG, "NAPT not in this IDF build; IP forwarding only");
#endif
	return 0;
}
