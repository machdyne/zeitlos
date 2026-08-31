#ifndef NET_PHY_H
#define NET_PHY_H

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Which NIC driver eth.c/net.c call -- chosen at RUNTIME, from the
 * feature CSR, with both drivers linked in.
 *
 * -- Why this changed --
 *
 * It used to be a build-time fork: sw/apps/net/Makefile's NET_PHY
 * variable picked one driver, #defined the phy_* names onto it, and
 * left the other out of the link. That worked, but it made net the
 * only app in the tree that had to be built per board, and the cost
 * of that landed everywhere:
 *
 *   - sw/apps was otherwise board-agnostic, so the whole software
 *     half had to be rebuilt per target in a release even though
 *     nothing else in it varied.
 *   - the core app archive (sw/os/zar.h) differed per board for the
 *     sake of one object file.
 *   - net.bin could not ship on the sdcard image at all, because a
 *     card copy takes precedence over the flash copy and one card
 *     image would have handed half the boards the wrong driver.
 *   - this Makefile grew .net_phy_selected, a stamp file existing
 *     purely to notice that NET_PHY had changed without any .c file
 *     changing -- see its comment for the link error and the quieter
 *     silent-misconfiguration that motivated it.
 *
 * -- What it costs --
 *
 * Measured, rv32im -Os with --gc-sections: 1112 bytes, about 1.3% of
 * net.bin. The whole of rmii_eth.o is 972 of that and the dispatch
 * table and probe are the rest. That buys a binary that runs
 * correctly on every board, so it is not a close call.
 *
 * -- How selection works --
 *
 * POSITIVE detection, via z_soc_has_feature() rather than by negating
 * anything: a bitstream that says ETH_RMII gets the RMII driver, one
 * that says SPI_ETH gets the ENC28J60 driver.
 *
 * The order of the two checks does not matter, because no bitstream
 * sets both -- release/lib/spec.py rejects a target that tries, and
 * rtl/sysctl.v would need two MACs to honour it.
 *
 * The interesting case is the third one: a bitstream predating
 * rtl/csrs.v cannot answer at all, and there "unknown" must not be
 * read as "absent". Such a build gets the ENC28J60 driver, which is
 * what NET_PHY defaulted to before this existed, so old bitstreams
 * behave exactly as they did. Only a CSR-capable bitstream that
 * positively reports neither NIC makes net exit -- which is what lets
 * sw/os/sh.c's `init` start net unconditionally on every board.
 */

#include <stdbool.h>
#include <stdint.h>

#include "enc28j60.h"
#include "rmii_eth.h"

typedef struct {
	const char *name;
	bool (*init)(const uint8_t mac[6]);
	uint16_t (*recv)(uint8_t *buf, uint16_t maxlen);
	bool (*send)(const uint8_t *buf, uint16_t len);
	void (*debug_dump)(void);
} net_phy_t;

// The active driver. NULL until net_phy_select() has run, which
// net.c's main() does before anything touches the hardware.
extern const net_phy_t *net_phy;

// Picks a driver from the feature CSR. Returns NULL when this
// bitstream positively reports no ethernet hardware; net.c exits
// cleanly on that rather than probing registers that are not there.
const net_phy_t *net_phy_select(void);

// Call-site spellings kept from the #define era, so eth.c and the rest
// of net.c did not have to change when this became a runtime choice.
#define NET_PHY_NAME     (net_phy->name)
#define phy_init(mac)    (net_phy->init(mac))
#define phy_recv(b, l)   (net_phy->recv((b), (l)))
#define phy_send(b, l)   (net_phy->send((b), (l)))
#define phy_debug_dump() (net_phy->debug_dump())

#endif
