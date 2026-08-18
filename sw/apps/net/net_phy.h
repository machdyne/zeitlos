#ifndef NET_PHY_H
#define NET_PHY_H

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Single source of truth for which NIC driver eth.c/net.c actually
 * call -- selected at BUILD time by sw/apps/net/Makefile's NET_PHY
 * variable (default ENC28J60), not a runtime probe.
 *
 * Why build-time and not runtime: sw/bios is already built per-board
 * (Makefile passes -DBOARD_$(BOARD)), but sw/apps isn't -- the top-
 * level `apps` target builds one binary set with no board awareness,
 * because until now nothing under sw/apps needed to know. This is
 * the first app-level hardware fork, and a runtime probe would need
 * to answer "was rtl/ethmac_rmii.v actually synthesized into this
 * bitstream?" from a register read alone -- which doesn't work: an
 * unmapped address in sysctl.v's wishbone mux just returns whatever
 * that mux's default case resolves to (see rmii_eth.c's header
 * comment), not a reliably-absent value distinguishable from real
 * hardware in some particular state. Simplest correct answer: pick
 * the driver when you build for the board, same as -DBOARD_ already
 * does one level down. Build for mozart_ml1 with `make -C
 * sw/apps/net NET_PHY=RMII`; every other currently-supported board
 * keeps the default (ENC28J60).
 */

#ifdef NET_PHY_RMII

#include "rmii_eth.h"
#define NET_PHY_NAME     "rmii"
#define phy_init         rmii_eth_init
#define phy_recv         rmii_eth_recv
#define phy_send         rmii_eth_send
#define phy_debug_dump   rmii_eth_debug_dump

#else

#include "enc28j60.h"
#define NET_PHY_NAME     "enc28j60"
#define phy_init         enc28j60_init
#define phy_recv         enc28j60_recv
#define phy_send         enc28j60_send
#define phy_debug_dump   enc28j60_debug_dump

#endif

#endif
