/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * SOC feature table -- the human-readable half of zsoc.h's Z_FEATURE_*
 * bits.
 *
 * ============================================================
 *  KEEP THIS FILE IN SYNC WITH rtl/sysctl.v's CSR_FEATURES
 * ============================================================
 *
 * There is no shared source of truth between the Verilog and C sides
 * (same split as rtl/usb_hid.v and sw/common/zkbd.h for HID usage
 * translation) -- bit position is the only thing that has to match, and
 * nothing checks that it does. A shifted or missing bit shows up as a
 * board reporting hardware it doesn't have, or omitting hardware it
 * demonstrably does, and nothing else will complain.
 *
 * This table lives HERE, next to the Z_FEATURE_* defines in zsoc.h,
 * specifically so that everything needing to track the RTL is in one
 * place: adding a feature is one bit in zsoc.h and one row here, both
 * in the same directory, rather than a bit in a header and a table
 * buried in whichever consumer happened to want to print it first.
 *
 * Deliberately DATA ONLY -- no printing. Formatting belongs to whoever
 * is displaying it (currently k_soc_report() in sw/os/kernel.c, which
 * groups these into indented lines for the boot log); a shared file
 * that pulled in printf() would be unusable from any context that
 * doesn't have stdio, and would bake one consumer's layout choices into
 * everyone else's.
 *
 * Rows MUST stay sorted by `group`. Consumers walk this in order and
 * start a new line whenever the group changes, so an out-of-order row
 * produces a duplicated group heading rather than any kind of error.
 */

#include <stdint.h>

#include "zsoc.h"

const z_feature_info_t z_soc_features[] = {

	{ Z_FEATURE_CPU_MUL,      "mul",    Z_FEAT_GROUP_CPU  },
	{ Z_FEATURE_CPU_MUL_FAST, "mul-hw", Z_FEAT_GROUP_CPU  },
	{ Z_FEATURE_CPU_DIV,      "div",    Z_FEAT_GROUP_CPU  },

	{ Z_FEATURE_MEM_SRAM,   "sram",    Z_FEAT_GROUP_MEMORY  },
	{ Z_FEATURE_MEM_SDRAM,  "sdram",   Z_FEAT_GROUP_MEMORY  },
	{ Z_FEATURE_MEM_VRAM,   "vram",    Z_FEAT_GROUP_MEMORY  },
	{ Z_FEATURE_MEM_QQSPI,  "qqspi",   Z_FEAT_GROUP_MEMORY  },
	{ Z_FEATURE_MEM_ROM,    "rom",     Z_FEAT_GROUP_MEMORY  },
	{ Z_FEATURE_MEM_GLYPH,  "glyph",   Z_FEAT_GROUP_MEMORY  },

	{ Z_FEATURE_GPU,        "gpu",     Z_FEAT_GROUP_GPU     },
	{ Z_FEATURE_GPU_RASTER, "raster",  Z_FEAT_GROUP_GPU     },
	{ Z_FEATURE_GPU_BLIT,   "blit",    Z_FEAT_GROUP_GPU     },
	{ Z_FEATURE_GPU_CURSOR, "cursor",  Z_FEAT_GROUP_GPU     },
	{ Z_FEATURE_GPU_VGA,    "vga",     Z_FEAT_GROUP_GPU     },
	{ Z_FEATURE_GPU_DDMI,   "ddmi",    Z_FEAT_GROUP_GPU     },

	{ Z_FEATURE_UART0,      "uart0",   Z_FEAT_GROUP_INPUT   },
	{ Z_FEATURE_USB_HID,    "usb-hid", Z_FEAT_GROUP_INPUT   },

	{ Z_FEATURE_SPI_SDCARD, "sdcard",  Z_FEAT_GROUP_STORAGE },
	{ Z_FEATURE_SPI_FLASH,  "flash",   Z_FEAT_GROUP_STORAGE },

	{ Z_FEATURE_SPI_ETH,    "spi-eth", Z_FEAT_GROUP_NETWORK },
	{ Z_FEATURE_ETH_RMII,   "rmii",    Z_FEAT_GROUP_NETWORK },

	{ Z_FEATURE_LED_RGB,    "rgb",     Z_FEAT_GROUP_LED     },
	{ Z_FEATURE_LED_DEBUG,  "debug",   Z_FEAT_GROUP_LED     },

};

const int z_soc_features_count =
	(int)(sizeof(z_soc_features) / sizeof(z_soc_features[0]));

// Padded to a common width so a consumer printing them in a column
// doesn't have to. Indexed by Z_FEAT_GROUP_*, so this array's order is
// fixed by that enum, not by anything here.
const char *const z_soc_feature_groups[] = {
	"cpu    ",
	"memory ",
	"gpu    ",
	"input  ",
	"storage",
	"network",
	"led    ",
};
