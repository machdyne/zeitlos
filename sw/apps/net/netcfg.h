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

typedef struct {
	int has_file;
	int has_wifi;
	int dhcp;           /* -1 unset, 0/1 from file */
	uint32_t ip;        /* 0 = unset */
	uint32_t mask;
	uint32_t gw;
	uint32_t dns;
	char ssid[NETCFG_SSID_MAX + 1];
	char psk[NETCFG_PSK_MAX + 1];
} netcfg_t;

/* 0 even if the file is missing (has_file=0). -1 on a malformed file. */
int netcfg_load(netcfg_t *out);

#endif
