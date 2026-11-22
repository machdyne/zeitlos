#ifndef NETCFG_H
#define NETCFG_H

/*
 * Parse /data/net/net.cfg. See docs/esp32link.md.
 *
 * ssid/psk  -> ESP32 STA (ZNIC_STA)
 * dhcp/ip/mask/gw/dns -> net.c IP stack
 * phy=auto|usb|builtin -> which NIC (docs/usb_ethernet.md)
 * nfs=address:/export -> mounted at /nfs (docs/nfs.md)
 */

#include <stdint.h>

#define NETCFG_SSID_MAX 32
#define NETCFG_PSK_MAX  63
#define NETCFG_NFS_MAX  127

/* How many ssid=/psk= pairs net.cfg may list, in order of preference.
 * The backend tries them first to last and takes the first one whose
 * network actually exists. */
#define NETCFG_WIFI_MAX 4

typedef struct {
	char ssid[NETCFG_SSID_MAX + 1];
	char psk[NETCFG_PSK_MAX + 1];
} netcfg_wifi_t;

#ifdef NETX
#define NETCFG_IF_MAX      3
#define NETCFG_ROUTES_MAX  8
typedef struct {
	char name[6];       /* wan, lan, lan2 */
	int phy;            /* -1 unset, else NET_PHY_* */
	int dhcp;           /* -1 unset */
	uint32_t ip, mask, gw;
	int nat, dhcpd;     /* 0/1 */
	char allow[64];     /* services let in from outside (fw.h) */
	uint32_t pool_lo, pool_hi, lease;
} netcfg_if_t;
#define NETCFG_PORTFWD_MAX 16
#endif

typedef struct {
	int has_file;
	int has_wifi;
	int dhcp;           /* -1 unset, 0/1 from file */
	/* phy= : which NIC to use. 0 auto (a MAC in the bitstream if there
	 * is one, else a USB CDC-ECM adapter), 1 usb, 2 builtin. See
	 * net_phy.h's NET_PHY_* and docs/usb_ethernet.md. */
	int phy;
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
	/* nfs=address:/export -- mounted at /nfs once there is an
	 * address (nfs.c, docs/nfs.md); empty: none */
	char nfs[NETCFG_NFS_MAX + 1];
	/* full = 0: flash net, even with netx on the card (docs/netx.md) */
	int full;
#ifdef NETX
	/* netx: wan., lan., lan2. (docs/netx.md) */
	int n_if;
	netcfg_if_t ifs[NETCFG_IF_MAX];
	int forward;
	int n_routes;
	struct { uint32_t net, mask, via; } routes[NETCFG_ROUTES_MAX];
	int nat_size;       /* 0: the default */
	int dnsfwd;         /* -1 unset (on where there is a DHCP server) */
	int n_portfwd;
	struct { uint8_t proto; uint16_t wan_port, lan_port; uint32_t lan_ip; } portfwd[NETCFG_PORTFWD_MAX];
#endif
} netcfg_t;

/* 0 even if the file is missing (has_file=0). -1 on a malformed file. */
int netcfg_load(netcfg_t *out);

// "a.b.c.d" into a host-order address; 0 on success, -1 otherwise.
int netcfg_parse_ipv4(const char *s, uint32_t *out);

#endif
