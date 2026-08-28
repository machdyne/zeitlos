#include "tftpd.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "tftpd";

#define TFTP_BLOCK      512
#define TFTP_RETRIES    5
#define TFTP_ACK_MS     2000
#define TFTP_DATA_MS    5000
#define OP_RRQ 1
#define OP_WRQ 2
#define OP_DATA 3
#define OP_ACK 4
#define OP_ERROR 5

/* ---- sources ------------------------------------------------------ */

typedef struct {
	int kind;			/* 0 synthetic, 1 http */
	uint32_t size, off;		/* synthetic */
	esp_http_client_handle_t http;	/* http */
	int http_eof;
} source_t;

static int http_open(source_t *src, const char *url)
{
	esp_http_client_config_t cfg = {
		.url = url,
		.timeout_ms = 15000,
		.crt_bundle_attach = esp_crt_bundle_attach,
		.disable_auto_redirect = true,	/* handled below, per hop */
		.buffer_size = 1024,
	};
	src->http = esp_http_client_init(&cfg);
	if (!src->http)
		return -1;
	esp_http_client_set_header(src->http, "Accept-Encoding", "identity");
	esp_http_client_set_header(src->http, "User-Agent", "zeitlos-nic/1");
	for (int hop = 0; hop < 4; hop++) {
		esp_err_t e = esp_http_client_open(src->http, 0);
		if (e != ESP_OK) {
			ESP_LOGW(TAG, "http open: %s", esp_err_to_name(e));
			return -1;
		}
		int64_t clen = esp_http_client_fetch_headers(src->http);
		int status = esp_http_client_get_status_code(src->http);
		ESP_LOGI(TAG, "http status %d len %lld", status, (long long)clen);
		if (status == 301 || status == 302 || status == 303 ||
				status == 307 || status == 308) {
			esp_http_client_set_redirection(src->http);
			esp_http_client_close(src->http);
			continue;
		}
		if (status != 200) {
			esp_http_client_close(src->http);
			return -1;
		}
		src->http_eof = 0;
		return 0;
	}
	ESP_LOGW(TAG, "http: too many redirects");
	return -1;
}

/* fills up to `want` bytes; returns bytes (< want only at end) or -1 */
static int source_read(source_t *src, uint8_t *buf, int want)
{
	if (src->kind == 0) {
		int n = 0;
		while (n < want && src->off < src->size) {
			buf[n++] = (uint8_t)((src->off * 7 + 3) ^ (src->off >> 8));
			src->off++;
		}
		return n;
	}
	int n = 0;
	while (n < want && !src->http_eof) {
		int r = esp_http_client_read(src->http, (char *)buf + n, want - n);
		if (r < 0)
			return -1;
		if (r == 0) {
			src->http_eof = 1;
			break;
		}
		n += r;
		src->off += r;
	}
	return n;
}

static void source_close(source_t *src)
{
	if (src->kind == 1 && src->http) {
		esp_http_client_close(src->http);
		esp_http_client_cleanup(src->http);
		src->http = NULL;
	}
}

static int source_open(source_t *src, const char *name)
{
	memset(src, 0, sizeof(*src));
	if (!strncmp(name, "http://", 7) || !strncmp(name, "https://", 8)) {
		src->kind = 1;
		ESP_LOGI(TAG, "GET %s (free heap %lu)", name,
			(unsigned long)esp_get_free_heap_size());
		return http_open(src, name);
	}
	src->kind = 0;
	if (!strcmp(name, "test.bin"))
		src->size = 32 * 1024;
	else if (!strcmp(name, "big.bin"))
		src->size = 256 * 1024;
	else
		return -1;
	return 0;
}

/* ---- protocol ----------------------------------------------------- */

static void send_error(int s, const struct sockaddr_in *to, int code, const char *msg)
{
	uint8_t pkt[4 + 64];
	pkt[0] = 0; pkt[1] = OP_ERROR; pkt[2] = 0; pkt[3] = (uint8_t)code;
	size_t m = strlen(msg);
	if (m > 60) m = 60;
	memcpy(pkt + 4, msg, m);
	pkt[4 + m] = 0;
	sendto(s, pkt, 5 + m, 0, (const struct sockaddr *)to, sizeof(*to));
}

/* wait for a packet from `from` within ms; returns length or -1 */
static int wait_pkt(int s, uint8_t *buf, int cap, struct sockaddr_in *from, int ms)
{
	fd_set r;
	FD_ZERO(&r);
	FD_SET(s, &r);
	struct timeval tv = { .tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000 };
	if (select(s + 1, &r, NULL, NULL, &tv) <= 0)
		return -1;
	struct sockaddr_in a;
	socklen_t al = sizeof(a);
	int n = recvfrom(s, buf, cap, 0, (struct sockaddr *)&a, &al);
	if (n < 4)
		return -1;
	if (a.sin_addr.s_addr != from->sin_addr.s_addr || a.sin_port != from->sin_port)
		return -1;	/* someone else; ignore */
	return n;
}

static void serve_rrq(struct sockaddr_in *client, const char *name)
{
	source_t src;
	static uint8_t pkt[4 + TFTP_BLOCK];
	static uint8_t rx[4 + TFTP_BLOCK];
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0)
		return;
	struct sockaddr_in me = { .sin_family = AF_INET, .sin_port = 0, .sin_addr.s_addr = INADDR_ANY };
	bind(s, (struct sockaddr *)&me, sizeof(me));

	if (source_open(&src, name) != 0) {
		send_error(s, client, 1, "not found / fetch failed");
		close(s);
		return;
	}
	uint16_t block = 1;
	uint32_t total = 0;
	int ok = 0;
	for (;;) {
		int n = source_read(&src, pkt + 4, TFTP_BLOCK);
		if (n < 0) {
			send_error(s, client, 0, "read error");
			break;
		}
		pkt[0] = 0; pkt[1] = OP_DATA; pkt[2] = block >> 8; pkt[3] = block & 0xff;
		int tries = 0, acked = 0;
		while (tries++ <= TFTP_RETRIES) {
			sendto(s, pkt, 4 + n, 0, (const struct sockaddr *)client, sizeof(*client));
			int r = wait_pkt(s, rx, sizeof(rx), client, TFTP_ACK_MS);
			if (r >= 4 && rx[1] == OP_ACK && ((rx[2] << 8) | rx[3]) == block) {
				acked = 1;
				break;
			}
			if (r >= 4 && rx[1] == OP_ERROR)
				break;
		}
		if (!acked)
			break;
		total += n;
		if (n < TFTP_BLOCK) {
			ok = 1;
			break;
		}
		block++;
	}
	ESP_LOGI(TAG, "RRQ '%s' %s: %lu bytes, %u blocks", name, ok ? "done" : "FAILED",
		(unsigned long)total, block);
	source_close(&src);
	close(s);
}

static void serve_wrq(struct sockaddr_in *client, const char *name)
{
	static uint8_t rx[4 + TFTP_BLOCK];
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0)
		return;
	struct sockaddr_in me = { .sin_family = AF_INET, .sin_port = 0, .sin_addr.s_addr = INADDR_ANY };
	bind(s, (struct sockaddr *)&me, sizeof(me));
	uint8_t ack[4] = { 0, OP_ACK, 0, 0 };
	uint16_t expect = 1;
	uint32_t total = 0;
	int ok = 0;
	sendto(s, ack, 4, 0, (const struct sockaddr *)client, sizeof(*client));
	for (;;) {
		int r = wait_pkt(s, rx, sizeof(rx), client, TFTP_DATA_MS);
		if (r < 0)
			break;
		if (rx[1] != OP_DATA)
			continue;
		uint16_t b = (rx[2] << 8) | rx[3];
		ack[2] = rx[2]; ack[3] = rx[3];
		sendto(s, ack, 4, 0, (const struct sockaddr *)client, sizeof(*client));
		if (b != expect)
			continue;	/* duplicate: re-acked above */
		total += r - 4;
		expect++;
		if (r - 4 < TFTP_BLOCK) {
			ok = 1;
			break;
		}
	}
	ESP_LOGI(TAG, "WRQ '%s' %s: %lu bytes received", name, ok ? "done" : "FAILED",
		(unsigned long)total);
	close(s);
}

static void tftpd_task(void *arg)
{
	(void)arg;
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in me = { .sin_family = AF_INET, .sin_port = htons(69), .sin_addr.s_addr = INADDR_ANY };
	if (s < 0 || bind(s, (struct sockaddr *)&me, sizeof(me)) != 0) {
		ESP_LOGE(TAG, "bind udp/69 failed");
		vTaskDelete(NULL);
		return;
	}
	ESP_LOGI(TAG, "listening on udp/69");
	static uint8_t req[4 + 256];
	for (;;) {
		struct sockaddr_in client;
		socklen_t cl = sizeof(client);
		int n = recvfrom(s, req, sizeof(req) - 1, 0, (struct sockaddr *)&client, &cl);
		if (n < 4)
			continue;
		req[n] = 0;
		int op = (req[0] << 8) | req[1];
		const char *name = (const char *)req + 2;
		size_t nl = strnlen(name, n - 2);
		if (nl == 0 || nl >= (size_t)(n - 2))
			continue;
		const char *mode = name + nl + 1;
		ESP_LOGI(TAG, "%s '%s' mode=%s from %s", op == OP_RRQ ? "RRQ" : op == OP_WRQ ? "WRQ" : "?",
			name, mode, inet_ntoa(client.sin_addr));
		if (op == OP_RRQ)
			serve_rrq(&client, name);
		else if (op == OP_WRQ)
			serve_wrq(&client, name);
		else
			send_error(s, &client, 4, "illegal op");
		/* requests that queued up while we were busy are the client's
		 * retries of the one just served (an http fetch can take
		 * longer than its first-block patience): drop them, or every
		 * retry would start another fetch */
		int stale = 0;
		while (recvfrom(s, req, sizeof(req) - 1, MSG_DONTWAIT,
				(struct sockaddr *)&client, &cl) > 0)
			stale++;
		if (stale)
			ESP_LOGI(TAG, "dropped %d stale request(s)", stale);
	}
}

void tftpd_start(void)
{
	/* HTTP(S) needs a fat stack: TLS handshake + http client */
	xTaskCreate(tftpd_task, "tftpd", 12288, NULL, 6, NULL);
}
