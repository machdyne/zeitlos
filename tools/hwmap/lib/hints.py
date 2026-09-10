#
# Zeitlos hwmap -- display hints.
#
# COSMETIC ONLY. Nothing in this file decides what is on the map, what is
# connected to what, or what is optional; that all comes from the RTL.
# A module with no entry here is still drawn, under its module name, and
# `hwmap` prints a note suggesting you add one.
#
#   name      what the block is called on the map
#   short     a narrower form for tight boxes (defaults to name)
#   cat       colour category, see CATEGORIES
#   doc       docs/<doc>.md, listed on page 2
#   params    instance parameters worth printing, with a format; the
#             VALUE always comes from the instantiation in sysctl.v
#   window    (addr_param, mask_param) of a module that maps a virtual
#             window -- both values are read from the module's RTL
#

CATEGORIES = {
    #  key        label                fill       stroke
    "cpu":     ("CPU / bus",           (0.86, 0.89, 0.95), (0.30, 0.38, 0.55)),
    "mem":     ("Memory",              (0.84, 0.94, 0.90), (0.18, 0.45, 0.36)),
    "video":   ("Video / GPU",         (0.91, 0.88, 0.97), (0.40, 0.32, 0.62)),
    "io":      ("I/O / HID",           (0.98, 0.92, 0.82), (0.58, 0.40, 0.12)),
    "net":     ("Storage / network",   (0.85, 0.92, 0.98), (0.16, 0.38, 0.60)),
    "audio":   ("Audio",               (0.98, 0.88, 0.92), (0.60, 0.25, 0.40)),
    "sys":     ("System / crypto",     (0.92, 0.92, 0.89), (0.38, 0.38, 0.34)),
    "clock":   ("Clocking",            (0.93, 0.93, 0.93), (0.40, 0.40, 0.40)),
    "unknown": ("Unclassified",        (1.00, 1.00, 1.00), (0.35, 0.35, 0.35)),
}

MODULES = {
    "picorv32_wb":     dict(name="PicoRV32", cat="cpu", doc=None),
    "zeitlos32_wb":    dict(name="Zeitlos32", cat="cpu", doc="zeitlos32"),
    "wb_mtu":          dict(name="MTU", cat="cpu", doc=None,
                            window=("TRANSLATE_ADDR", "TRANSLATE_MASK")),
    "wb_icache":       dict(name="Instruction cache", short="Icache", cat="cpu",
                            doc="icache",
                            params={"CACHE_KB": "{} KB"}),
    "wb_arbiter_main": dict(name="Main bus arbiter", short="main arbiter",
                            cat="cpu", doc=None),
    "wb_arbiter_vram": dict(name="VRAM arbiter", short="VRAM arbiter",
                            cat="cpu", doc=None),
    "USRMCLK":         dict(name="ECP5 config-clock primitive", cat="clock"),
    "bram_wb":         dict(name="Boot BRAM", cat="mem", doc="boot"),
    "sram_wb":         dict(name="SRAM", cat="mem"),
    "sdram_wb":        dict(name="SDRAM", cat="mem"),
    "qqspi_wb":        dict(name="PSRAM", cat="mem"),
    "vram_wb":         dict(name="VRAM", cat="mem"),
    "spiflashro_wb":   dict(name="SPI flash", cat="mem", doc="flash_apps"),
    "glyph_mem":       dict(name="Glyph RAM", cat="mem", doc="gpu_blitter",
                            params={"ADDR_WIDTH": "2^{} bytes"}),
    "gpio_wb":         dict(name="GPIO + LEDs", short="GPIO", cat="io",
                            doc="gpio"),
    "uart_wb":         dict(name="UART 16550", short="UART", cat="io",
                            doc="uart"),
    "uart_null":       dict(name="UART stub", cat="io", doc="uart"),
    "usb_cdc_uart":    dict(name="USB CDC console", short="USB CDC", cat="io",
                            doc="usb_cdc"),
    "usb_hid_wb":      dict(name="USB HID host", short="USB HID", cat="io",
                            doc="user_input"),
    "esp32_rxfifo":    dict(name="ESP32 RX FIFO", short="ESP32 RX", cat="net",
                            doc="esp32link"),
    "spim_wb":         dict(name="SPI master", cat="net", doc="spi"),
    "ethmac_rmii_wb":  dict(name="Ethernet MAC", short="Eth MAC", cat="net",
                            doc="networking"),
    "socctl_wb":       dict(name="SoC control", cat="sys", doc="socctl"),
    "csrs_wb":         dict(name="Capability CSRs", short="CSRs", cat="sys",
                            doc="csrs"),
    "rtc_wb":          dict(name="RTC", cat="sys", doc="rtc"),
    "trng_wb":         dict(name="TRNG", cat="sys", doc="trng"),
    "montmul":         dict(name="Montgomery mult.", short="Montmul", cat="sys",
                            doc="montmul", params={"LIMBS": "{} limbs"}),
    "audio_wb":        dict(name="Audio", cat="audio", doc="audio",
                            params={"DEPTH_LOG2": "FIFO 2^{}"}),
    "gpu_raster_wb":   dict(name="Line rasterizer", short="Raster", cat="video",
                            doc="gpu_raster"),
    "gpu_blit_wb":     dict(name="Blitter", cat="video", doc="gpu_blitter"),
    "gpu_video":       dict(name="Video out", cat="video", doc="composite"),
    "gpu_cursor":      dict(name="HW cursor", cat="video", doc=None),
    "pll0":            dict(name="PLL0", cat="clock"),
    "pll1":            dict(name="PLL1", cat="clock"),
    "pll0_25":         dict(name="PLL0 (25M in)", cat="clock"),
    "pll1_25":         dict(name="PLL1 (25M in)", cat="clock"),
    "CC_PLL":          dict(name="GateMate PLL", cat="clock"),
}

# Children (instances inside a module) worth naming in a block.
CHILDREN = {
    "audio_mixer": "mixer",
    "audio_out": "PWM/I2S out",
    "audio_spdif": "S/PDIF",
    "usb_hid_host": "HID host core",
    "gpu_ddmi": "DDMI (TMDS) encoder",
    "usb_cdc": "USB CDC core",
}

# Pin-group labels. Matched against the generated label (a prefix plus
# "_*", or a single port name).
PINS = {
    "sdram_*": "SDRAM",
    "SRAM_*": "SRAM",
    "QQSPI_*": "QSPI PSRAM",
    "SD_*": "microSD",
    "ETH_*": "Ethernet",
    "VGA_*": "VGA",
    "DDMI_*": "HDMI (DDMI)",
    "COMP_DAC": "composite DAC",
    "usb_host_*": "USB host",
    "usb_ufp_*": "USB-C device",
    "UART0_*": "UART0",
    "UART1_*": "UART1",
    "UART1_RX": "UART1 RX",
    "CSPI_*": "config flash",
    "CSPI_SCK": "config flash",
    "LED_R": "trap LED",
    "LED_G": "IRQ LED",
    "wifi_en_*": "ESP32 control",
    "AUDIO_*": "PWM audio L/R",
    "AUD_*": "PT8211 DAC",
    "AUD_OPTICAL": "S/PDIF optical",
    "GPIO0": "GPIO0",
    "GPIO1": "GPIO1",
    "GPIO2": "GPIO2",
    "GPIO3": "GPIO3",
    "LED_*": "LEDs",
    "LED_B": "LED",
    "DBG": "debug LEDs",
}

# Interface prefixes worth a word on the map.
IFACES = {
    "s_": "src reads",
    "mx_": "sample fetch",
    "m_": "VRAM writes",
    "wbm_": "",
    "c_": "",
}


# Names for slaves implemented inline in sysctl.v rather than as a
# module instance, keyed by their decode wire.
INLINE = {
    "cs_esp32ctl": ("ESP32 ctl", "net"),
}


class Hints:
    def __init__(self):
        self.missing = set()

    def mod(self, module):
        h = MODULES.get(module)
        if h is None:
            self.missing.add(module)
            return {}
        return h

    def name(self, b):
        return self.mod(b.module).get("name", b.module)

    def short(self, b):
        h = self.mod(b.module)
        return h.get("short", h.get("name", b.module))

    def cat(self, b):
        return self.mod(b.module).get("cat", "unknown")

    def doc(self, b):
        d = self.mod(b.module).get("doc")
        return ("docs/%s.md" % d) if d else None

    def params(self, b, defaults):
        out = []
        spec = self.mod(b.module).get("params", {})
        for p, fmt in spec.items():
            if p not in b.params:
                continue
            txt, _ = b.params[p]
            if txt.startswith("`"):
                # the map shows the effective default; the macro name is in
                # the feature index
                dv = defaults.get(txt[1:])
                if dv is None:
                    continue
                txt = dv[0]
            out.append(fmt.format(txt))
        return out

    def pins(self, label):
        return PINS.get(label, label)

    def child(self, module):
        return CHILDREN.get(module)

    def inline(self, decode):
        name = INLINE.get(decode, (decode[3:] if decode.startswith("cs_") else decode,))[0]
        return name + " (inline)"

    def inline_cat(self, decode):
        ent = INLINE.get(decode)
        return ent[1] if ent and len(ent) > 1 else "unknown"

    def iface(self, prefix):
        return IFACES.get(prefix, prefix.rstrip("_"))
