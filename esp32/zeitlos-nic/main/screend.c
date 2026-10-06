#include "screend.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_httpd_priv.h"	/* httpd_ws_respond_server_handshake, ESP-IDF 5.4 */
#include "znic.h"
#include "lwip/sockets.h"

static const char *TAG = "screend";

#define STRIPES      30
#define STRIPE_BYTES 1280
#define SCREEN_PORT  7777
#define MAX_CLIENTS  8

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[]   asm("_binary_index_html_end");

#define STRIPE_MAX (STRIPE_BYTES + 64)	/* PackBits worst case per stripe */
static uint8_t shadow[STRIPES * STRIPE_MAX];
static uint16_t shadow_len[STRIPES];	/* compressed bytes held per stripe */
static volatile uint32_t dirty_all;	/* stripes changed since last relay */
static SemaphoreHandle_t lock;
static SemaphoreHandle_t wake;	/* given by udp_task when a stripe lands */

static httpd_handle_t server;
static struct {
	int fd;			/* -1 = free */
	uint32_t pending;	/* stripes this client still needs */
	uint32_t seen;		/* tick of last successful send -- staleness */
} clients[MAX_CLIENTS];

/* Tell net how many viewers there are: ZNIC_VIEWERS {n:u8}. net cannot
 * see the WebSocket, and this is what it decides whether to stream on.
 * Sent whenever n changes and again with every keepalive round
 * (always = 1), so a lost message, or a net that has just started, is
 * put right within a second. The periodic one waits while the control
 * queue holds anything: whatever is in it goes first, and a net that
 * is not polling does not get its queue filled with them. */
static void viewers_announce(int always)
{
	static int last = -1;
	int n = 0;
	xSemaphoreTake(lock, portMAX_DELAY);
	for (int i = 0; i < MAX_CLIENTS; i++)
		if (clients[i].fd >= 0)
			n++;
	xSemaphoreGive(lock);
	if (n == last && !(always && znic_ctl_depth() == 0))
		return;
	uint8_t b = (uint8_t)n;
	if (znic_ctl_push(ZNIC_VIEWERS, &b, 1) == 0)
		last = n;
}

static void udp_task(void *arg)
{
	(void)arg;
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in me = { .sin_family = AF_INET,
		.sin_port = htons(SCREEN_PORT), .sin_addr.s_addr = INADDR_ANY };
	if (s < 0 || bind(s, (struct sockaddr *)&me, sizeof(me)) != 0) {
		ESP_LOGE(TAG, "bind udp/%d failed", SCREEN_PORT);
		vTaskDelete(NULL);
		return;
	}
	static uint8_t buf[6 + STRIPE_MAX];
	for (;;) {
		int n = recv(s, buf, sizeof(buf), 0);
		if (n < 6 || buf[0] != 'Z' || buf[1] != 'S')
			continue;
		int idx = buf[2];
		int clen = n - 6;			/* PackBits payload */
		if (idx >= STRIPES || clen > STRIPE_MAX)
			continue;
		xSemaphoreTake(lock, portMAX_DELAY);
		memcpy(shadow + idx * STRIPE_MAX, buf + 6, clen);
		shadow_len[idx] = (uint16_t)clen;
		dirty_all |= 1u << idx;
		xSemaphoreGive(lock);
		xSemaphoreGive(wake);	/* relay now, do not wait out a tick */
	}
}

static esp_err_t root_get(httpd_req_t *req)
{
	httpd_resp_set_type(req, "text/html");
	return httpd_resp_send(req, index_html_start,
		index_html_end - index_html_start);
}

/* Same rule as zd_origin_ok() in sw/apps/zerdesk/deskcfg.h. A missing
 * Origin is taken: a tool is not a browser, and a page cannot omit the
 * header. A present Origin must be http:// or https://, either case,
 * followed by the Host header and nothing else. */
static int origin_ok(const char *origin, int olen, const char *host, int hlen)
{
	int sn = 0;
	int i;

	if (!origin)
		return 1;
	if (olen < 1 || !host || hlen < 1)
		return 0;
	if (olen >= 8) {
		sn = 8;
		for (i = 0; i < 8; i++) {
			char a = origin[i];
			if (a >= 'A' && a <= 'Z')
				a = (char)(a + 32);
			if (a != "https://"[i]) {
				sn = 0;
				break;
			}
		}
	}
	if (!sn && olen >= 7) {
		sn = 7;
		for (i = 0; i < 7; i++) {
			char a = origin[i];
			if (a >= 'A' && a <= 'Z')
				a = (char)(a + 32);
			if (a != "http://"[i]) {
				sn = 0;
				break;
			}
		}
	}
	if (!sn)
		return 0;
	return olen - sn == hlen &&
		memcmp(origin + sn, host, (size_t)hlen) == 0;
}

static int upgrade_origin_ok(httpd_req_t *req)
{
	char origin[160];
	char host[160];
	esp_err_t err;

	err = httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof origin);
	if (err == ESP_ERR_NOT_FOUND)
		return 1;
	if (err != ESP_OK)
		return 0;
	err = httpd_req_get_hdr_value_str(req, "Host", host, sizeof host);
	if (err != ESP_OK)
		return 0;
	return origin_ok(origin, (int)strlen(origin), host, (int)strlen(host));
}

static esp_err_t ws_refuse(httpd_req_t *req, unsigned code)
{
	uint8_t body[2];
	httpd_ws_frame_t close = {
		.final = true,
		.type = HTTPD_WS_TYPE_CLOSE,
		.payload = body,
		.len = 2,
	};

	body[0] = (uint8_t)(code >> 8);
	body[1] = (uint8_t)code;
	httpd_ws_send_frame(req, &close);
	return ESP_FAIL;
}

/* an input frame from the browser: {usage, mods, pressed}, relayed
 * to Zeitlos as a ZNIC_INPUT control message (znic.c). A frame that is
 * not 3 or 5 bytes is closed: 1009 past 125 bytes, 1002 otherwise.
 * Leaving it unread would desynchronise the parser. */
static esp_err_t ws_frame(httpd_req_t *req)
{
	httpd_ws_frame_t f = { 0 };
	uint8_t ev[5];
	int want;

	if (httpd_ws_recv_frame(req, &f, 0) != ESP_OK)
		return ws_refuse(req, 1002);
	if (f.len > 125)
		return ws_refuse(req, 1009);
	if (f.len != 3 && f.len != 5)
		return ws_refuse(req, 1002);
	want = (int)f.len;
	f.payload = ev;
	if (httpd_ws_recv_frame(req, &f, want) != ESP_OK)
		return ws_refuse(req, 1002);
	/* push it to Zeitlos unsolicited (not via the poll queue):
	 * it lands in the FPGA's receive FIFO, which raises the
	 * cpu_irq[8] that wakes net at once -- input no longer
	 * waits for net's next poll. See esp32_rxfifo.v's rx_ready
	 * and docs/remote_desktop.md. */
	znic_send(want == 5 ? ZNIC_MOUSE : ZNIC_INPUT, ev, want);
	return ESP_OK;
}

static esp_err_t ws_upgrade(httpd_req_t *req)
{
	struct httpd_req_aux *aux = req->aux;
	int fd, slot, evicted;

	if (!upgrade_origin_ok(req)) {
		ESP_LOGW(TAG, "upgrade refused: origin not allowed");
		httpd_resp_set_status(req, "403 Forbidden");
		httpd_resp_set_type(req, "text/plain");
		httpd_resp_set_hdr(req, "Cache-Control", "no-store");
		httpd_resp_set_hdr(req, "Connection", "close");
		httpd_resp_send(req, "Origin not allowed\n", HTTPD_RESP_USE_STRLEN);
		return ESP_OK;
	}
	if (httpd_ws_respond_server_handshake(req, NULL) != ESP_OK) {
		httpd_resp_set_status(req, "400 Bad Request");
		httpd_resp_set_type(req, "text/plain");
		httpd_resp_send(req, "400 Bad Request\n", HTTPD_RESP_USE_STRLEN);
		return ESP_OK;
	}
	aux->sd->ws_handshake_done = true;
	aux->sd->ws_handler = ws_frame;
	aux->sd->ws_control_frames = false;
	aux->sd->ws_user_ctx = NULL;

	fd = httpd_req_to_sockfd(req);
	xSemaphoreTake(lock, portMAX_DELAY);
	slot = -1;
	evicted = -1;
	for (int i = 0; i < MAX_CLIENTS; i++)
		if (clients[i].fd < 0) { slot = i; break; }
	if (slot < 0) {		/* table full: evict the oldest */
		slot = 0;
		for (int i = 1; i < MAX_CLIENTS; i++)
			if (clients[i].seen < clients[slot].seen) slot = i;
		evicted = clients[slot].fd;
	}
	clients[slot].fd = fd;
	clients[slot].pending = (1u << STRIPES) - 1;
	clients[slot].seen = xTaskGetTickCount();
	xSemaphoreGive(lock);
	/* net counts viewers from these two lines: every slot that is
	 * freed has to say so, whichever path frees it */
	if (evicted >= 0)
		ESP_LOGI(TAG, "viewer gone (fd %d)", evicted);
	ESP_LOGI(TAG, "viewer connected (fd %d)", fd);
	viewers_announce(0);
	return ESP_OK;
}

static void relay_task(void *arg)
{
	(void)arg;
	/* one WS frame carries every pending stripe: [idx:u8, len:u16le,
	 * data...] repeated. 30 small frames become one. */
	static uint8_t out[STRIPES * (3 + STRIPE_MAX)];
	for (;;) {
		/* wake the instant a stripe lands; 33ms backstop covers the
		 * forced full pass and anything missed */
		xSemaphoreTake(wake, pdMS_TO_TICKS(33));
		if (!server)
			continue;
		/* every ~1s, ping idle clients so a dead-but-synced one is
		 * noticed and its slot freed (the page ignores a 1-byte frame) */
		static uint32_t last_ping;
		uint32_t nowt = xTaskGetTickCount();
		if (nowt - last_ping > pdMS_TO_TICKS(1000)) {
			last_ping = nowt;
			uint8_t ka = 0xff;	/* idx 0xff -> no stripe, ignored */
			httpd_ws_frame_t kf = { .final = true,
				.type = HTTPD_WS_TYPE_BINARY, .payload = &ka, .len = 1 };
			for (int c = 0; c < MAX_CLIENTS; c++) {
				if (clients[c].fd < 0 || clients[c].pending) continue;
				if (httpd_ws_send_frame_async(server, clients[c].fd, &kf) != ESP_OK) {
					ESP_LOGI(TAG, "viewer gone (fd %d)", clients[c].fd);
					clients[c].fd = -1; clients[c].pending = 0;
				}
			}
			viewers_announce(1);
		}
		xSemaphoreTake(lock, portMAX_DELAY);
		uint32_t newly = dirty_all;
		dirty_all = 0;
		for (int i = 0; i < MAX_CLIENTS; i++)
			if (clients[i].fd >= 0)
				clients[i].pending |= newly;
		xSemaphoreGive(lock);
		for (int c = 0; c < MAX_CLIENTS; c++) {
			if (clients[c].fd < 0 || !clients[c].pending)
				continue;
			int o = 0;
			xSemaphoreTake(lock, portMAX_DELAY);
			for (int idx = 0; idx < STRIPES; idx++) {
				if (!(clients[c].pending & (1u << idx))) continue;
				int ml = shadow_len[idx];
				if (ml == 0) { clients[c].pending &= ~(1u << idx); continue; }
				out[o++] = (uint8_t)idx;
				out[o++] = (uint8_t)(ml & 0xff);
				out[o++] = (uint8_t)(ml >> 8);
				memcpy(out + o, shadow + idx * STRIPE_MAX, ml);
				o += ml;
				clients[c].pending &= ~(1u << idx);
			}
			xSemaphoreGive(lock);
			if (!o) continue;
			httpd_ws_frame_t f = {
				.final = true,
				.type = HTTPD_WS_TYPE_BINARY,
				.payload = out,
				.len = o,
			};
			if (httpd_ws_send_frame_async(server,
					clients[c].fd, &f) != ESP_OK) {
				ESP_LOGI(TAG, "viewer gone (fd %d)", clients[c].fd);
				clients[c].fd = -1;
				clients[c].pending = 0;
				viewers_announce(0);
			} else {
				clients[c].seen = xTaskGetTickCount();
			}
		}
	}
}

void screend_start(void)
{
	lock = xSemaphoreCreateMutex();
	wake = xSemaphoreCreateBinary();
	for (int i = 0; i < MAX_CLIENTS; i++)
		clients[i].fd = -1;

	httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
	cfg.lru_purge_enable = true;
	if (httpd_start(&server, &cfg) != ESP_OK) {
		ESP_LOGE(TAG, "httpd_start failed");
		return;
	}
	static const httpd_uri_t root = {
		.uri = "/", .method = HTTP_GET, .handler = root_get };
	static const httpd_uri_t ws = {
		.uri = "/ws", .method = HTTP_GET, .handler = ws_upgrade,
		.is_websocket = false };
	httpd_register_uri_handler(server, &root);
	httpd_register_uri_handler(server, &ws);

	xTaskCreate(udp_task, "scr_udp", 4096, NULL, 7, NULL);
	xTaskCreate(relay_task, "scr_ws", 4096, NULL, 5, NULL);
	ESP_LOGI(TAG, "desktop on http://<station-ip>/ (ws relay, udp %d)",
		SCREEN_PORT);
}
