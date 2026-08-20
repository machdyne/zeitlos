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
 * the first app-level hardware fork, and picking WHICH DRIVER CODE
 * gets compiled in genuinely has to happen at build time regardless
 * (ENC28J60 and RMII are different drivers with different APIs, not
 * two configurations of the same one) -- same reasoning -DBOARD_
 * already applies one level down, for the BIOS.
 *
 * What CAN now happen at runtime, and does (net.c's own main()):
 * checking whether the SPECIFIC board this is actually running on was
 * built with the ethernet backend this binary was compiled for, via
 * rtl/csrs.v's feature CSR (sw/common/zsoc.h,
 * z_soc_feature_confirmed_absent()) -- see docs/csrs.md. This used to
 * be impossible: an unmapped address in sysctl.v's wishbone mux just
 * returns whatever that mux's default case resolves to, not a
 * reliably-absent value distinguishable from real hardware in some
 * particular state (see zsoc.h's own header comment) -- so net had no
 * way to tell "this board genuinely has no ethernet hardware" from
 * "it does, and this register just isn't ready yet" without CSRs.
 * That's why net.c used to hang forever on a board like Lakritz
 * (neither SPI_ETH nor ETH_RMII) instead of failing cleanly, and why
 * sw/os/sh.c's `init` used to only reserve net's pid rather than
 * starting it -- both fixed now that net can check first.
 *
 * Build for mozart_ml1 with `make -C sw/apps/net NET_PHY=RMII`; every
 * other currently-supported board keeps the default (ENC28J60).
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
