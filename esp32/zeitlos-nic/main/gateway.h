#ifndef GATEWAY_H
#define GATEWAY_H

#include <stdint.h>
#include <stddef.h>

#define GATEWAY_SCAN_MAX 32

typedef struct {
	char ssid[33];
	int rssi;
	int ch;
	int auth;
} gateway_ap_t;

int  gateway_init(void);
void gateway_wait_ready(void);
int  gateway_wifi_scan(const char *want_ssid, gateway_ap_t *out, int maxn);
int  gateway_wifi_sta(const char *ssid, const char *psk, int *rssi);
int  gateway_from_zeitlos(const uint8_t *frame, uint16_t len);
int  gateway_pop_to_zeitlos(uint8_t *out, uint16_t *len);
void gateway_znic_mac(uint8_t out[6]);
void gateway_stats(uint32_t *from_z, uint32_t *to_z, uint32_t *dropped);
int  gateway_last_disc_reason(void);
int  gateway_last_scan_found(void);
int  gateway_sta_got_ip(void);
uint32_t gateway_sta_ip(void);

#endif
