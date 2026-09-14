#ifndef NETCFG_H
#define NETCFG_H

/*
 * Parse /NET.CFG on the FAT root (8.3, FF_USE_LFN=0). See docs/esp32-net.md.
 *
 * ssid/psk  -> ESP32 STA (ZNIC_STA)
 * dhcp/ip/mask/gw/dns -> net.c IP stack
 */

#include <stdint.h>

#define NETCFG_SSID_MAX 32
#define NETCFG_PSK_MAX  63

/* How many ssid=/psk= pairs NET.CFG may list, in order of preference.
 * The backend tries them first to last and takes the first one whose
 * network actually exists. */
#define NETCFG_WIFI_MAX 4

typedef struct {
	char ssid[NETCFG_SSID_MAX + 1];
	char psk[NETCFG_PSK_MAX + 1];
} netcfg_wifi_t;

typedef struct {
	int has_file;
	int has_wifi;
	int dhcp;           /* -1 unset, 0/1 from file */
	uint32_t ip;        /* 0 = unset */
	uint32_t mask;
	uint32_t gw;
	uint32_t dns;
	/* entry 0 mirrored here so existing "which network" prints keep
	 * working; wifi[]/n_wifi is the full ordered list */
	char ssid[NETCFG_SSID_MAX + 1];
	char psk[NETCFG_PSK_MAX + 1];
	int n_wifi;
	netcfg_wifi_t wifi[NETCFG_WIFI_MAX];
} netcfg_t;

/* 0 even if the file is missing (has_file=0). -1 on a malformed file. */
int netcfg_load(netcfg_t *out);

#endif
