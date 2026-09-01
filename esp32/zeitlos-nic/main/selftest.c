/*
 * UART0 self-test CLI. Speaks ZTEST lines so a host script can drive
 * scan/STA/ping without Zeitlos or an SD card.
 */

#include "selftest.h"
#include "gateway.h"
#include "znic.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/ip_addr.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "ping/ping_sock.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static volatile int ping_replies;
static SemaphoreHandle_t ping_done;

static void zprint(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	printf("ZTEST ");
	vprintf(fmt, ap);
	printf("\n");
	fflush(stdout);
	va_end(ap);
}

static void on_ping_success(esp_ping_handle_t hdl, void *args)
{
	(void)args;
	uint32_t elapsed = 0;
	esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed, sizeof(elapsed));
	ping_replies++;
	zprint("ping reply rtt_ms=%lu", (unsigned long)elapsed);
}

static void on_ping_timeout(esp_ping_handle_t hdl, void *args)
{
	(void)hdl;
	(void)args;
	zprint("ping timeout");
}

static void on_ping_end(esp_ping_handle_t hdl, void *args)
{
	(void)args;
	uint32_t sent = 0, recv = 0;
	esp_ping_get_profile(hdl, ESP_PING_PROF_REQUEST, &sent, sizeof(sent));
	esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY, &recv, sizeof(recv));
	zprint("ping sent=%lu recv=%lu", (unsigned long)sent, (unsigned long)recv);
	xSemaphoreGive(ping_done);
}

static int do_ping(const char *host)
{
	ip_addr_t target;
	memset(&target, 0, sizeof(target));
	if (!ipaddr_aton(host, &target)) {
		struct addrinfo hint = { 0 }, *res = NULL;
		hint.ai_family = AF_INET;
		if (getaddrinfo(host, NULL, &hint, &res) != 0 || !res)
			return -1;
		struct sockaddr_in *a = (struct sockaddr_in *)res->ai_addr;
		ip_addr_set_ip4_u32_val(target, a->sin_addr.s_addr);
		freeaddrinfo(res);
	}

	esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
	cfg.task_stack_size = 4096;	/* default 2048 overflows with the log callbacks */
	cfg.count = 3;
	cfg.interval_ms = 400;
	cfg.timeout_ms = 1000;
	cfg.target_addr = target;

	ping_replies = 0;
	xSemaphoreTake(ping_done, 0);
	esp_ping_callbacks_t cbs = {
		.on_ping_success = on_ping_success,
		.on_ping_timeout = on_ping_timeout,
		.on_ping_end = on_ping_end,
	};
	esp_ping_handle_t hdl;
	if (esp_ping_new_session(&cfg, &cbs, &hdl) != ESP_OK)
		return -1;
	esp_ping_start(hdl);
	int ok = xSemaphoreTake(ping_done, pdMS_TO_TICKS(8000)) == pdTRUE;
	esp_ping_delete_session(hdl);
	if (!ok)
		return -1;
	return ping_replies > 0 ? 0 : -1;
}

static void dump_scan(const char *want)
{
	gateway_ap_t aps[GATEWAY_SCAN_MAX];
	int n = gateway_wifi_scan(want, aps, GATEWAY_SCAN_MAX);
	for (int i = 0; i < n; i++)
		zprint("ap ssid='%s' rssi=%d ch=%d auth=%d",
			aps[i].ssid, aps[i].rssi, aps[i].ch, aps[i].auth);
	zprint("scan n=%d found=%d want='%s'",
		n, gateway_last_scan_found(), want ? want : "");
}

static void cmd_sta(const char *ssid, const char *psk)
{
	int rssi = 0;
	if (gateway_wifi_sta(ssid, psk, &rssi) != 0) {
		zprint("sta fail reason=%d scan=%d",
			gateway_last_disc_reason(), gateway_last_scan_found());
		return;
	}
	uint32_t ip = gateway_sta_ip();
	zprint("sta ip=%lu.%lu.%lu.%lu rssi=%d",
		(unsigned long)((ip >> 24) & 0xff),
		(unsigned long)((ip >> 16) & 0xff),
		(unsigned long)((ip >> 8) & 0xff),
		(unsigned long)(ip & 0xff),
		rssi);
}

static void cmd_test(const char *ssid, const char *psk)
{
	zprint("begin ssid='%s'", ssid);
	dump_scan(ssid);
	if (!gateway_last_scan_found()) {
		zprint("result=FAIL fail=scan");
		return;
	}
	cmd_sta(ssid, psk);
	if (!gateway_sta_got_ip()) {
		zprint("result=FAIL fail=sta reason=%d", gateway_last_disc_reason());
		return;
	}
	if (do_ping("8.8.8.8") != 0 && do_ping("1.1.1.1") != 0) {
		zprint("result=FAIL fail=ping");
		return;
	}
	zprint("result=PASS");
}

static char *next_tok(char **sp)
{
	char *s = *sp;
	while (*s == ' ' || *s == '\t')
		s++;
	if (!*s) {
		*sp = s;
		return NULL;
	}
	char *tok = s;
	while (*s && *s != ' ' && *s != '\t')
		s++;
	if (*s) {
		*s = 0;
		s++;
	}
	*sp = s;
	return tok;
}

/* "inject": feed the gateway the same frame Zeitlos sends for a DNS
 * query (192.168.4.2 -> 8.8.8.8 UDP/53, example.com A) so the
 * forward + NAPT path can be exercised -- and crash -- with UART0
 * attached. */
static uint16_t csum16(const uint8_t *p, int n)
{
	uint32_t s = 0;
	for (int i = 0; i + 1 < n; i += 2)
		s += ((uint32_t)p[i] << 8) | p[i + 1];
	if (n & 1)
		s += (uint32_t)p[n - 1] << 8;
	while (s >> 16)
		s = (s & 0xffff) + (s >> 16);
	return (uint16_t)~s;
}

static const uint8_t ZMAC[6] = { 0x02, 0, 0, 0, 0, 0x01 };	/* Zeitlos */

static void ip_hdr(uint8_t *ip, uint16_t iplen, uint8_t proto, const uint8_t dst[4])
{
	memset(ip, 0, 20);
	ip[0] = 0x45; ip[2] = iplen >> 8; ip[3] = iplen & 0xff;
	ip[4] = 0x12; ip[5] = 0x34; ip[8] = 64; ip[9] = proto;
	ip[12] = 192; ip[13] = 168; ip[14] = 4; ip[15] = 2;
	memcpy(ip + 16, dst, 4);
	uint16_t c = csum16(ip, 20);
	ip[10] = c >> 8; ip[11] = c & 0xff;
}

/* answer an ARP request for 192.168.4.2 the way Zeitlos's net would */
static void arp_autoreply(const uint8_t *req, uint16_t n)
{
	if (n < 42 || req[12] != 0x08 || req[13] != 0x06 || req[21] != 1)
		return;
	if (!(req[38] == 192 && req[39] == 168 && req[40] == 4 && req[41] == 2))
		return;
	uint8_t r[42];
	memcpy(r, req + 6, 6);		/* to the asker */
	memcpy(r + 6, ZMAC, 6);
	r[12] = 0x08; r[13] = 0x06;
	r[14] = 0; r[15] = 1; r[16] = 8; r[17] = 0; r[18] = 6; r[19] = 4; r[20] = 0; r[21] = 2;
	memcpy(r + 22, ZMAC, 6);
	r[28] = 192; r[29] = 168; r[30] = 4; r[31] = 2;
	memcpy(r + 32, req + 22, 10);	/* target = sender of the request */
	gateway_from_zeitlos(r, 42);
	zprint("inject: answered ARP for 192.168.4.2");
}

static void inject(const char *what)
{
	static const uint8_t dns[] = {
		0xbe, 0xef, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		7, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 3, 'c', 'o', 'm', 0,
		0x00, 0x01, 0x00, 0x01
	};
	static const uint8_t GW[4] = { 192, 168, 4, 1 };
	static const uint8_t GOOGLE[4] = { 8, 8, 8, 8 };
	uint8_t f[128];
	uint16_t len;

	gateway_znic_mac(f);			/* dst: the gateway's MAC */
	memcpy(f + 6, ZMAC, 6);
	f[12] = 0x08; f[13] = 0x00;
	uint8_t *ip = f + 14;

	if (!strcmp(what, "dns")) {
		uint16_t ulen = 8 + sizeof(dns);
		ip_hdr(ip, 20 + ulen, 17, GOOGLE);
		uint8_t *udp = ip + 20;
		udp[0] = 0xcf; udp[1] = 0x08;	/* 53000 */
		udp[2] = 0; udp[3] = 53;
		udp[4] = ulen >> 8; udp[5] = ulen & 0xff;
		udp[6] = 0; udp[7] = 0;		/* no UDP checksum, like Zeitlos */
		memcpy(udp + 8, dns, sizeof(dns));
		len = 14 + 20 + ulen;
	} else {	/* "gw" or "ping": ICMP echo, 32 bytes of payload */
		ip_hdr(ip, 20 + 8 + 32, 1, !strcmp(what, "gw") ? GW : GOOGLE);
		uint8_t *ic = ip + 20;
		memset(ic, 0, 40);
		ic[0] = 8; ic[4] = 0x12; ic[5] = 0x34; ic[6] = 0; ic[7] = 1;
		for (int i = 0; i < 32; i++) ic[8 + i] = (uint8_t)i;
		uint16_t c = csum16(ic, 40);
		ic[2] = c >> 8; ic[3] = c & 0xff;
		len = 14 + 20 + 40;
	}
	int r = gateway_from_zeitlos(f, len);
	zprint("inject %s len=%u r=%d", what, len, r);
	for (int i = 0; i < 50; i++) {
		vTaskDelay(pdMS_TO_TICKS(100));
		uint8_t out[1600];
		uint16_t n = 0;
		while (gateway_pop_to_zeitlos(out, &n)) {
			zprint("inject reply len=%u type=%02x%02x proto=%u src=%u.%u.%u.%u",
				n, out[12], out[13], (n >= 34) ? out[23] : 0,
				(n >= 34) ? out[26] : 0, (n >= 34) ? out[27] : 0,
				(n >= 34) ? out[28] : 0, (n >= 34) ? out[29] : 0);
			arp_autoreply(out, n);
		}
	}
}

static void handle_line(char *line)
{
	char *p = line;
	while (*p == ' ' || *p == '\t')
		p++;
	int n = (int)strlen(p);
	while (n > 0 && (p[n - 1] == '\n' || p[n - 1] == '\r'))
		p[--n] = 0;
	if (!*p || *p == '#')
		return;

	char *rest = p;
	char *cmd = next_tok(&rest);
	if (!cmd)
		return;

	if (!strcmp(cmd, "help")) {
		zprint("cmds=scan,sta <ssid> <psk>,ping <host>,ip,test <ssid> <psk>");
	} else if (!strcmp(cmd, "sta2")) {
		char *ss = next_tok(&rest);
		char *pk = rest ? rest : "";
		if (!ss) { zprint("usage: sta2 <ssid> <psk>"); return; }
		zprint("sta2 -> wifi_task r=%d", nic_start_sta(ss, pk));
	} else if (!strcmp(cmd, "poll")) {
		char *a = next_tok(&rest);
		int n = a ? atoi(a) : 20;
		zprint("ctl depth=%u", znic_ctl_depth());
		for (int i = 0; i < n; i++) {
			znic_ctl_t c;
			if (!znic_ctl_pop(&c)) {
				zprint("ctl empty after %d", i);
				break;
			}
			zprint("ctl type=%02x len=%u: %.*s", c.type, c.len,
				(c.type == ZNIC_LOG) ? (int)c.len : 0, (const char *)c.payload);
		}
	} else if (!strcmp(cmd, "inject")) {
		char *what = next_tok(&rest);
		inject(what ? what : "dns");
	} else if (!strcmp(cmd, "scan")) {
		char *want = next_tok(&rest);
		dump_scan(want);
	} else if (!strcmp(cmd, "sta")) {
		char *ssid = next_tok(&rest);
		while (*rest == ' ' || *rest == '\t')
			rest++;
		if (!ssid || !*rest) {
			zprint("usage sta <ssid> <psk>");
			return;
		}
		cmd_sta(ssid, rest);
	} else if (!strcmp(cmd, "ping")) {
		char *host = next_tok(&rest);
		if (!host) {
			zprint("usage ping <host>");
			return;
		}
		if (do_ping(host) != 0)
			zprint("ping fail host='%s'", host);
		else
			zprint("ping ok host='%s'", host);
	} else if (!strcmp(cmd, "ip")) {
		if (!gateway_sta_got_ip()) {
			zprint("ip none");
			return;
		}
		uint32_t ip = gateway_sta_ip();
		zprint("ip %lu.%lu.%lu.%lu",
			(unsigned long)((ip >> 24) & 0xff),
			(unsigned long)((ip >> 16) & 0xff),
			(unsigned long)((ip >> 8) & 0xff),
			(unsigned long)(ip & 0xff));
	} else if (!strcmp(cmd, "test")) {
		char *ssid = next_tok(&rest);
		while (*rest == ' ' || *rest == '\t')
			rest++;
		if (!ssid) {
			zprint("usage test <ssid> <psk>");
			return;
		}
		cmd_test(ssid, rest);
	} else {
		zprint("unknown cmd='%s'", cmd);
	}
}

static void selftest_task(void *arg)
{
	(void)arg;
	setvbuf(stdin, NULL, _IONBF, 0);
	setvbuf(stdout, NULL, _IONBF, 0);
	zprint("ready");
	char line[192];
	for (;;) {
		if (!fgets(line, sizeof(line), stdin)) {
			vTaskDelay(pdMS_TO_TICKS(50));
			continue;
		}
		handle_line(line);
	}
}

void selftest_start(void)
{
	if (!ping_done)
		ping_done = xSemaphoreCreateBinary();
	xTaskCreate(selftest_task, "ztest", 12288, NULL, 4, NULL);
}
