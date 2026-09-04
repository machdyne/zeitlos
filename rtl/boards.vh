`ifndef ZEITLOS_BOARDS_VH
`define ZEITLOS_BOARDS_VH

// UNIVERSAL CONFIG
// ----------------

// `DEBUG IS GONE. It used to guard rtl/debug.v, the block at
// 0xe000_0000 that owns the board LEDs. That block is now rtl/gpio.v
// and rtl/sysctl.v instantiates it unconditionally, the same way it
// does rtl/csrs.v and rtl/socctl.v.
//
// The define was universal, so removing it changes no board's
// gateware. What it removes is a branch that would have been fatal if
// anyone had ever taken it: without `DEBUG nothing decoded the 0xE
// nibble, an undecoded address gets no ack on this bus, and the first
// LED write in sw/bios/bios.c would have stalled the CPU before a
// single character of the boot banner. That was tolerable while the
// block held two LED bits nobody probes; it is not now that software
// reads a MAGIC there to ask whether GPIO exists. See rtl/gpio.v.

`define ARBITER

// RTC: the wall clock (rtl/rtc.v) -- seconds since the Unix epoch plus
// a 1/1024s fraction, set over the network by sw/apps/net's SNTP
// client and read by sw/apps/clock. See docs/rtc.md.
//
// Universal rather than per-board because there is no board-specific
// reason to want it or not want it: it needs no pins, no external
// part and no board support of any kind, just a prescaler and a
// counter clocked from sys_clk. Every board can have one, so every
// board does by default.
//
// Comment it out to reclaim the logic on a board that is genuinely
// tight -- roughly a 32-bit counter, a 24-bit prescaler and a small
// register file. Software copes: rtl/csrs.v's own FEATURE bit (see
// rtl/csrs.vh) goes clear, z_rtc_available() (sw/common/zrtc.h)
// answers false, net skips its NTP client entirely and sw/apps/clock
// says on screen that this bitstream has no clock. Nothing hangs and
// nothing has to be rebuilt differently -- unlike `CPU_MUL above,
// this is not a switch the software half has to agree with.
//
// NOT the same thing as rtl/sysctl.v's `rtc_ctr`, despite the name
// they share. That is the ~732Hz KTIMER divider, it counts ticks
// since boot, it is unconditional, and it is unaffected by this
// define. Only one of the two knows what a date is.
`define RTC

// rtl/trng.v -- a ring-oscillator true random number generator.
// Universal, for the same reason `RTC is: it needs no pins, no
// external part and no board support of any kind, just LUTs and a
// counter on sys_clk. Every board gets one unless somebody
// deliberately comments it out.
//
// A build without it is not a build software has to be told about:
// the FEATURE bit in rtl/csrs.v goes clear, rtl/sysctl.v hands the
// 0x7000_04xx window to csrs.v (which acks it and reads back zero),
// and z_rng_secure() (sw/common/zrng.h) answers false -- so the SSH
// client refuses to connect rather than generating a key from
// something guessable, and `(random)` in Scheme keeps working from a
// clearly-labelled non-cryptographic fallback.
//
// THE ONE THING TO WATCH: this is a combinational loop, which is
// precisely what synthesis exists to remove. If a toolchain optimises
// the oscillators away the block still runs and still returns words,
// and they are worthless. rtl/trng.v carries keep attributes in three
// dialects and a continuous health monitor for exactly this reason --
// check the HEALTH bit on real hardware after any toolchain change,
// not just after a code change. See docs/trng.md.
`define TRNG

// Game mode: a 320x240 viewport over the same 640x480 framebuffer,
// pixel-doubled on scanout so the display timing never changes. See
// rtl/gpu/gpu_video.v's header for the full design and
// docs/game_mode.md for what software does with it.
//
// Universal rather than per-board, for the same reason `RTC and `TRNG
// above are: it needs no pins, no external part and no board support
// of any kind. It is not even a new block -- it is a loadable counter
// and an adder inside rtl/gpu/gpu_video.v plus two registers in
// rtl/socctl.v, with no BRAM and no extra VRAM bandwidth (the scanline
// buffer already held a full framebuffer row; game mode just indexes
// it differently). Every board that can scan out pixels can have this,
// so every board does.
//
// What it buys is not really "games". It is that a 640x480 desktop
// becomes usable on a TV: the whole desktop is still there, still
// running, still exactly where it was, and CTRL-ALT-ARROW moves the
// viewport around it. Switching in and out kills no apps and destroys
// no windows, because nothing about the framebuffer changes -- only
// which part of it the display is pointed at.
//
// For an actual full-screen game the same mechanism is a double
// buffer: 640x480 holds four non-overlapping 320x240 pages, a page
// flip is one register write, and it is adopted at a frame boundary
// so it cannot tear. The line rasterizer and the blitter need no RTL
// changes at all and can draw into any page, because as far as they
// are concerned there is still exactly one 640x480 1bpp surface.
//
// ON A BOARD WITHOUT `GPU THIS DOES NOTHING, and rtl/sysctl.v makes
// that explicit rather than leaving it implied: it ands `GAME with
// `GPU before handing socctl its GAME_AVAIL parameter, so the enable
// bit is forced low in hardware and reads back low. A board with no
// scanout cannot be talked into a scanout mode.
//
// Comment it out to reclaim the logic. Software copes the same way it
// does for every other optional feature here: the FEATURE bit in
// rtl/csrs.v goes clear, socctl's GAME register reports avail = 0,
// z_game_available() (sw/common/zsoc.h) answers false, and the window
// manager simply does not bind ALT-ESC. Nothing hangs and nothing has
// to be rebuilt differently.
`define GAME

// RV32IM: hardware multiply and divide (rtl/cpu/picorv32/picorv32.v's
// ENABLE_FAST_MUL/ENABLE_MUL/ENABLE_DIV). Universal rather than
// per-board because the alternative -- some boards with M, some
// without -- means the software has to be built differently per board
// too, and a bitstream/binary mismatch here is not a graceful failure:
// every `mul` becomes an illegal instruction. See docs/muldiv.md.
//
// `CPU_MUL_FAST uses the DSP-backed multiplier (2 cycles). It is the
// one to watch in the nextpnr timing report -- picorv32 instantiates
// picorv32_pcpi_fast_mul with EXTRA_MUL_FFS=0, i.e. a full unpipelined
// 32x32 multiply, which is the most likely thing in this design to
// limit Fmax. If timing gets tight, comment it out and leave `CPU_MUL
// defined: that selects the sequential shift-add multiplier instead
// (~32 cycles, still roughly an order of magnitude faster than the
// libgcc software routine it replaces) with no timing risk at all.
`define CPU_MUL
`define CPU_MUL_FAST
`define CPU_DIV

// CPU core selection. Undefined (the default) means picorv32, exactly
// as before. Defining this selects rtl/cpu/zeitlos32 instead -- an
// experimental in-house RV32IM core that implements the same
// interrupt ABI, so sw/bios/boot_picorv32.S and sw/bios/custom_ops.S
// are unchanged either way and no software needs rebuilding to switch.
//
// This is a ONE LINE switch on purpose: zeitlos32 is developed
// alongside everything else rather than as a branch, and being able
// to A/B the two cores against an otherwise identical bitstream is
// what makes a mystery bug tractable ("is it my scheduler or my
// core?" is an expensive question to keep asking).
//
// `CPU_MUL / `CPU_MUL_FAST / `CPU_DIV above apply to both cores.
// Note `CPU_MUL_FAST on GateMate: rtl/../Makefile passes -nomult to
// synth_gatemate, so the DSP multiplier lands in LUTs there. See
// docs/zeitlos32.md.
//
//`define CPU_ZEITLOS32


// AUDIO
// -----
//
// rtl/audio.v + rtl/audio_out.v. PER-BOARD, unlike `RTC and `TRNG
// above, because unlike those it needs pins and a DAC on the other end
// of them -- there is no such thing as audio on a board that isn't
// wired for it.
//
// Two independent switches. `AUDIO builds the block at all; `AUDIO_SD
// and `AUDIO_PT8211 say which output stage gets connected to pins.
// Defining `AUDIO with neither builds a block that plays into nothing,
// which is legal and occasionally useful in simulation but is not what
// anybody wants on hardware.
//
//   `AUDIO_SD       two 1-bit sigma-delta DACs   (AUDIO_L, AUDIO_R)
//   `AUDIO_PT8211   PT8211/TM8211 serial DAC     (AUD_BCK/WS/DIN)
//   `AUDIO_SPDIF    IEC 60958 transmitter        (AUD_OPTICAL)
//
// A board with `AUDIO_SPDIF should also set `AUDIO_RATE_RESET to 16.
//
// S/PDIF is 128 half-cells per frame, so the line runs at 128*fs, and
// from a 48MHz sys_clk the reachable rates are fs = 375000/N. No
// standard rate is among them -- 44.1kHz wants N=8.5034 and 48kHz
// wants 7.8125. N=8 gives 46875Hz with a half-cell of exactly 8
// cycles: 2.34% below 48kHz, well inside any receiver's capture range,
// and exact so there is no jitter at all. RATE=16 is that rate, and it
// must be even because a half-cell is rate/2. See rtl/audio_spdif.v.
//
// Both may be defined together on a board that has both; the mixer and
// FIFO are shared and only the last stage differs. rtl/audio_out.v
// always instantiates both and lets yosys prune whichever reaches no
// pin, so there is no cost to a board that has one.
//
// A build without `AUDIO is not a build software has to be told about,
// the same as `RTC and `TRNG: rtl/sysctl.v hands the 0x7000_05xx
// window to csrs.v, which acks it and reads back zero, the FEATURE bit
// goes clear and z_audio_present() (sw/common/zaudio.h) answers false.
// Nothing hangs.
//
// Optional overrides, both defaulted in rtl/sysctl.v if a board does
// not set them:
//
//   `AUDIO_FIFO_LOG2    FIFO depth is 2**this, in stereo frames.
//                       Default 7 (128 frames, 2.9ms at 44.1kHz).
//                       NOT free to raise -- see sysctl.v's own note.
//   `AUDIO_RATE_RESET   power-on sample rate divider, fs = 48MHz/(64*R).
//                       Default 8'd17 -> 44117.6Hz.
//   `AUDIO_CTRL_RESET   power-on CTRL. Default 8'h00. Set bit 2
//                       (SWAPLR) here if a board's PT8211 comes up
//                       with its channels reversed -- once, rather
//                       than in every app.
//
// `AUDIO_MIXER is SEPARATE from `AUDIO and is the expensive half.
//
//   `AUDIO alone          FIFO + DAC output stage. The CPU mixes.
//   + `AUDIO_MIXER        eight channels of hardware mixing
//                         (rtl/audio_mixer.v), a third master on
//                         rtl/arbiter_main.v, and no per-sample CPU
//                         work at all.
//
// Measured on Obst, TRELLIS_COMB of 24288, post-nextpnr:
//
//   no audio                   16410   67%
//   `AUDIO                     see docs/audio.md
//   `AUDIO + `AUDIO_MIXER      19196   79%
//
// 79% is where nextpnr's placer starts working visibly harder on this
// device. Comment `AUDIO_MIXER out to get the logic back and fall
// straight to software mixing -- sw/apps/mod detects which it has and
// says so at startup, so nothing breaks, it just costs CPU again.
//
// `AUDIO_MIXER_CH_BITS is the intermediate dial: log2 of the mixer's
// channel count, default 3 (eight channels). Setting it to 2 gives
// four, which is all a ProTracker MOD needs, and halves the mixer's
// per-channel register file while turning every 8:1 read mux into a
// 4:1. The sequencer is unchanged either way -- it was already
// time-multiplexed -- so this costs channels and nothing else.
//
// MEASURED, AND DISAPPOINTING: four channels is 18972 COMB / 78%,
// against 19196 / 79% for eight. 224 COMB out of the mixer's 1733.
// The cost is the sequencer datapath and the wide muxes, not the
// per-channel storage, and TRELLIS_RAMW does not move at all because a
// 4-deep array still occupies the same 16-deep LUT-RAM primitives.
// The dial works; it is just not worth turning. If logic is what you
// need back, turn `AUDIO_MIXER off instead.

// COMPOSITE VIDEO
// ---------------
//
// rtl/gpu/gpu_video.v's monochrome CVBS output -- one resistor ladder
// on `dac`, one 75R series resistor, one RCA socket. See
// docs/composite.md for the timing derivation and the ladder values.
//
// `GPU_COMPOSITE       build the composite timing and output stage
// `GPU_COMPOSITE_PAL   PAL 288p at 50Hz. Without it, NTSC 240p at 60Hz.
//
// MUTUALLY EXCLUSIVE WITH `GPU_VGA AND `GPU_DDMI, and this is enforced
// in rtl/sysctl.v rather than left to a board author to remember.
//
// Not because the pixel pipeline could not feed all three -- it could,
// they share hline and the refill -- but because the TIMING is
// different. A 15.7kHz line rate and a 31.5kHz line rate cannot come
// out of one set of counters, and running two sets means two scanline
// buffers and an arbiter on vram.v's single graphics port. That is a
// real feature; it is not this one.
//
// THE VIEWPORT IS NOT OPTIONAL ON A COMPOSITE BOARD. Composite is
// 320x240, always, and gpu_video.v's FIXED_VIEWPORT parameter makes
// that unconditional -- socctl's game bit is not consulted at all.
//
// That is a bandwidth fact, not a choice: drawing 640 distinct pixels
// across a 52us active line needs 12.6MHz of luma and the channel
// carries about 4.2 (NTSC) or 5.5 (PAL). A "640 wide" composite
// picture is a blur of the correct average brightness, not a picture.
// So on a TV, CTRL-ALT-ARROW is how the rest of the desktop is
// reached, and `GAME above stops being a nice extra and becomes the
// thing that makes the machine usable at all.
//
// OFF BY DEFAULT ON EVERY BOARD, and these two lines are the switch.
// They are not per-board because a board does not "have" composite the
// way it has a DAC or an ethernet PHY -- Lakritz has the four pins
// either way, and which of its two video outputs is built is a choice
// made per bitstream, not per board.
//
// Lakritz is the one board wired for it today: boards/lakritz_v0.lpf
// carries COMP_DAC[3:0] on P1/R1/P2/N4, commented out, waiting for
// this. Enabling composite there means uncommenting BOTH those pins
// and `GPU_COMPOSITE here, AND commenting out `GPU_DDMI in the Lakritz
// block below -- see that .lpf's own note.
//
//`define GPU_COMPOSITE
//`define GPU_COMPOSITE_PAL

// BOARD CONFIG
// ------------
//
// `MEM is total main RAM in megabytes -- read by rtl/csrs.v (see
// docs/csrs.md) into a runtime-readable register, so software
// (sw/bios/bios.c, sw/os/mem.c) can size itself off the real number
// instead of a hardcoded assumption that only ever matched Obst (the
// first board this ran on). If a board block below doesn't set it,
// rtl/sysctl.v defaults it to 1 (matching that original hardcoded
// assumption) rather than leaving it undefined -- see that file's own
// `ifndef MEM guard.
//
// -- The ZSPEC escape hatch --
//
// Defining ZSPEC replaces this whole per-board chain with a generated
// zspec.vh. Nothing in an ordinary build does that: without -DZSPEC
// everything below behaves exactly as it always has, and
// `make BOARD=lakritz flash` is unaffected.
//
// It exists for release/, which builds a TARGET rather than a board --
// lakritz_uart and lakritz_langkatze are the same board with different
// PMODs, so they need different define sets. Additive defines could
// have come in on the yosys command line, but a variant is not always
// a superset of its base: a PMOD that occupies the console pins means
// building WITHOUT `UART0, and there is no command-line way to remove
// a define. Replacing the block wholesale is the only mechanism that
// can express absence.
//
// release/hw/boards/*.spec carries a copy of each block below, and
// `release/zrelease check` diffs the two and fails on any difference,
// so the duplication is verified rather than trusted. The universal
// section ABOVE this comment is not duplicated and not replaceable --
// a spec cannot turn off `RTC or `CPU_MUL, because each of those has a
// reason above for being universal that a per-target choice would not
// change.
`ifdef ZSPEC
`include "zspec.vh"
`else

`ifdef BOARD_OBST

`define FPGA_ECP5
`define OSC48
`define MEM 1				// note that some Obst boards have 2MB SRAM
`define MEM_SRAM
`define MEM_VRAM
//`define MEM_QQSPI
`define MEM_ROM
`define MEM_GLYPH
`define LED_RGB
//`define LED_DEBUG
`define GPU
`define GPU_RASTER
`define GPU_BLIT
`define GPU_CURSOR
`define GPU_VGA
`define UART0
`define USB_HID
`define SPI_SDCARD
`define SPI_ETH
`define AUDIO
`define AUDIO_SD
`define AUDIO_MIXER

// USB CDC-ACM console on the USB-C socket (rtl/usb_cdc_uart.v,
// docs/usb_cdc.md). OFF by default; uncommenting it is the whole
// change, because the `undef at the bottom of this file removes
// `UART0 for you and rtl/sysctl.v then hands the 0xf000_00xx window
// to the USB device instead of to rtl/ext/uart16550.
//
// WHAT IT BUYS. The console stops needing a PMOD. Obst's USB-C port
// is wired straight to the FPGA through series resistors and, after
// the DFU bootloader hands over, does nothing but supply power --
// so this is a console on a socket that was already occupied by the
// cable you are already using, and PMOD A comes free. It also means
// a new user needs no USB-UART PMOD to see anything at all, which
// on a board whose first-run experience is a serial banner is worth
// more than the connector.
//
// WHAT IT COSTS on this board specifically, measured rather than
// estimated: rtl/ext/usb_cdc is 1257 LUT4 at `USB_CDC_MPS 8 and
// rtl/ext/uart16550 that it displaces is 583, so the net is roughly
// +700 LUT4 and ZERO block RAM -- which matters here, because Obst
// sits at 52 of 56 DP16KD before any of this. Every byte of buffer
// in that core is flip-flops.
//
// WHAT TO WATCH. This board is an ECP5 12F at ~70% TRELLIS_COMB with
// about 3% of margin on the 48MHz clock before the block is added.
// Check `make timing BOARD=obst` after building, and do not raise
// `USB_CDC_MPS here without re-checking it.
//
// THE BOOT BANNER BLOCKS until something opens the port -- that is
// deliberate and is how the banner survives at all with no buffer to
// hold it. It gives up after `USB_CDC_STALL_CYCLES so an unattended
// board still boots. See rtl/usb_cdc_uart.v's header.
//
// Uncommenting this does NOT free PMOD A on its own; it only stops
// the console needing it. To put GPIO there as well, uncomment
// `GPIO_PORT0 below and the PMOD A block in boards/obst_v0.lpf.
`define USB_CDC

// GPIO (rtl/gpio.v, docs/gpio.md) is OFF in the plain board build and
// deliberately so: Obst has two PMOD connectors and this block claims
// both of them already. PMOD A is the serial console (`UART0) and
// PMOD B is the Langkatze ethernet PMOD (`SPI_ETH), so there is no
// free connector for a GPIO port to land on, and uncommenting the
// line below WITHOUT also removing one of those puts two top-level
// ports on the same balls -- which nextpnr reports as a placement
// conflict rather than silently mis-building, but is still not a
// useful thing to hand somebody.
//
// The supported way to build a GPIO Obst is the release system, which
// exists precisely because a variant is not always a superset of its
// base (see the ZSPEC note above):
//
//     ./release/zrelease build obst_uart_gpio
//
// That target keeps the console on PMOD A, puts GPIO port 0 on PMOD B
// and drops `SPI_ETH, which a command-line -D cannot express.
//`define GPIO_PORT0

// UART1, a second 16550 at 0xf000_0100, is off for the same reason and
// with the same fix. rtl/sysctl.v has had the block, the decode and
// the UART1_TX/UART1_RX pins behind `ifdef UART1 all along -- it was
// only ever reachable on the ULX3S, where that UART is soldered to the
// on-board ESP32. There is nowhere on Obst for its pins to go without
// giving up PMOD B:
//
//     ./release/zrelease build obst_uart_uart1
//
// keeps the console on PMOD A, puts UART1 on PMOD B pins 2 and 3, and
// drops `SPI_ETH. See docs/uart1.md.
//
// NOTE that this target and obst_uart_gpio are alternatives: both want
// PMOD B, and Obst has two connectors.
//`define UART1

`elsif BOARD_LAKRITZ

`define FPGA_ECP5
`define OSC48
`define MEM 32
`define MEM_SDRAM
`define MEM_VRAM
`define MEM_ROM
`define MEM_GLYPH
`define ICACHE
`define ICACHE_KB 4
`define ICACHE_LINE_WORDS 4
`define GPU
`define GPU_RASTER
`define GPU_BLIT
`define GPU_CURSOR
`define GPU_DDMI
`define UART0
`define USB_HID
`define SPI_SDCARD
`define AUDIO
`define AUDIO_SD
`define AUDIO_MIXER

// USB CDC-ACM console on the USB-C socket (rtl/usb_cdc_uart.v,
// docs/usb_cdc.md). OFF by default; uncommenting it is the whole
// change, because the `undef at the bottom of this file removes
// `UART0 for you.
//
// THIS BOARD IS WHERE IT MATTERS MOST. Everything the GPIO note below
// says -- one PMOD connector, the console is on it, so GPIO and a
// console are mutually exclusive and lakritz_gpio has to give up the
// console to get eight pins -- stops being true the moment the
// console lives on the USB-C socket instead. It is not a swap any
// more. The machine keeps its console AND gains a PMOD.
//
// Lakritz has fabric to spare for it: an ECP5 25F against Obst's 12F.
// `USB_CDC_MPS could reasonably go to 16 or 32 here for a faster
// link, at 1463 or 1901 LUT4 against 1257 at the default 8 -- though
// 8 is already worth about what the 1 Mbaud console it replaces was
// worth, so there is no need unless something actually wants the
// bandwidth.
//
// The boot banner blocks until something opens the port, and gives up
// after `USB_CDC_STALL_CYCLES so an unattended board still boots. See
// rtl/usb_cdc_uart.v's header.
`define USB_CDC

// GPIO off in the plain board build, for the same reason as Obst above
// but harder: Lakritz has exactly ONE PMOD connector and the serial
// console is on it (boards/lakritz_v0.lpf puts UART0_TX/RX on B12/B13,
// which are PMOD_A2 and PMOD_A3). A GPIO port here is therefore not an
// addition, it is a swap, and the console goes away with it.
//
//     ./release/zrelease build lakritz_gpio
//
// builds exactly that: GPIO port 0 on PMOD A, `UART0 removed, and
// rtl/uart_null.v answering the console window so the BIOS and kernel
// print into a hole instead of hanging on a UART that is not there.
// The machine still comes up on HDMI with a keyboard, which is the
// only reason this is a sane thing to ship at all.
//`define GPIO_PORT0

// UART1 is not offered on Lakritz at all, and that is a board fact
// rather than an omission: it has ONE PMOD connector and the console
// is on it. A second serial port would mean no first one, which is a
// configuration nobody wants -- unlike lakritz_gpio, where giving up
// the console buys eight pins that HDMI and a keyboard cannot
// replace. Two serial ports and no console is just one serial port
// with extra steps.
//
// Obst has two connectors; see obst_uart_uart1 there.

`elsif BOARD_MOZART_ML1

`define FPGA_ECP5
`define OSC48
`define MEM 32
`define MEM_SDRAM
`define MEM_VRAM
`define MEM_ROM
`define MEM_GLYPH
`define ICACHE
`define ICACHE_KB 8
`define ICACHE_LINE_WORDS 4
`define GPU
`define GPU_RASTER
`define GPU_BLIT
`define GPU_CURSOR
`define GPU_DDMI
`define UART0
`define USB_HID
`define SPI_SDCARD
`define ETH_RMII
// See the note on ETH_RX_SLOTS under BOARD_SERGEI_ML1.
`define ETH_RX_SLOTS 4
`define AUDIO
`define AUDIO_PT8211
`define AUDIO_MIXER

`elsif BOARD_SERGEI_ML1

`define FPGA_ECP5
`define OSC48
`define MEM 32
`define MEM_SDRAM
`define MEM_VRAM
`define MEM_ROM
`define MEM_GLYPH
`define ICACHE
`define ICACHE_KB 8
`define ICACHE_LINE_WORDS 4
`define GPU
`define GPU_RASTER
`define GPU_BLIT
`define GPU_CURSOR
`define GPU_DDMI
`define UART0
`define USB_HID
`define SPI_SDCARD
`define ETH_RMII
`define ETH_RMII_DRIVE_REFCLK
// Frames the RMII receive FIFO holds, one full-size frame per slot.
// Power of two, 2 or more. 2KB of block RAM each -- cheap on ECP5-45,
// which is the only place RMII is built. See rtl/ethmac_rmii.v.
`define ETH_RX_SLOTS 4
`define AUDIO
`define AUDIO_SPDIF
`define AUDIO_MIXER
// 46875Hz -- the only rate whose S/PDIF half-cell is an exact whole
// number of sys_clk. See the `AUDIO_SPDIF note above.
`define AUDIO_RATE_RESET 8'd16

// GPIO on the 6-pin PMOD, four pins, off by default.
//
// TWO reasons it is not simply uncommentable here. The connector has
// four signal pins rather than eight, so the port is declared NARROW
// (rtl/sysctl.v) -- and pin 1 is A13, which is the optical S/PDIF
// output. `AUDIO_SPDIF has to go, and on this board that is the ONLY
// audio output, so the trade is optical audio or four GPIO pins.
//
//     ./release/zrelease build sergei_gpio
//
// does both. See docs/gpio.md.
//
// Software still sees an eight-bit port: DIR and OUT bits 4-7 exist
// and drive nothing, and IN bits 4-7 read 0 where a real floating pin
// would read 1 (the pull-ups). That asymmetry is the cost of not
// carrying a per-port pin-count register on every board for one
// connector on one board.
// A hand build ALSO needs boards/sergei_ml1.lpf edited -- comment out
// the AUD_OPTICAL LOCATE and uncomment the four GPIO ones. Removing
// `AUDIO_SPDIF here deletes the port but not the constraint, and a
// LOCATE naming a port that does not exist is only a warning, so the
// build completes with GPIO0[0] quietly not working. See that file.
//`define GPIO_PORT0
//`define GPIO_PORT0_NARROW

`elsif BOARD_LEBKUCHEN

`define FPGA_GATEMATE
`define OSC48
`define MEM 8
//`define MEM_QQSPI
//`define MEM_QQSPI_SINGLE
`define MEM_VRAM
`define GPU
`define GPU_RASTER
`define GPU_BLIT
`define GPU_CURSOR
`define GPU_VGA
`define UART0
`define USB_HID
`define SPI_SDCARD
`define SPI_FLASH

`elsif BOARD_KOLSCH

`define FPGA_GATEMATE
`define OSC48
`define MEM 64
`define MEM_SDRAM
`define MEM_VRAM
`define GPU
`define GPU_RASTER
`define GPU_BLIT
`define GPU_CURSOR
`define GPU_VGA
`define UART0
`define USB_HID
//`define SPI_SDCARD
//`define SPI_FLASH

`elsif BOARD_ULX3S

`define FPGA_ECP5
`define OSC25
`define MEM 32
`define MEM_SDRAM
`define MEM_VRAM
`define MEM_ROM
`define MEM_GLYPH
`define ICACHE
`define ICACHE_KB 4
`define ICACHE_LINE_WORDS 4
`define GPU
`define GPU_RASTER
`define GPU_BLIT
`define GPU_CURSOR
`define GPU_DDMI
`define UART0
`define UART1
`define USB_HID
// Pointer sensitivity: divide mouse deltas by 32. Modern mice report
// thousands of counts per inch -- enough that at 1:1 a nudge crosses
// this 640-pixel screen, and enough that hand tremor while moving
// sideways shows up as tens of pixels of vertical wander. Lower this
// for a mouse that reports fewer counts. See usb_hid_wb's SENS_SHIFT.
`define USB_HID_SENS_SHIFT 3
`define SPI_SDCARD
`define ESP32_LINK

`define AUDIO
`define AUDIO_SPDIF
`define AUDIO_MIXER
// 46875Hz -- see the `AUDIO_SPDIF note above. ULX3S runs from a 25MHz
// crystal rather than 48, but pll0_25 produces sys_clk of EXACTLY
// 48.0000 MHz (480MHz VCO / 10), so the S/PDIF arithmetic is identical
// to the 48MHz-crystal boards and the half-cell is still 8 cycles.
`define AUDIO_RATE_RESET 8'd16

`endif

`endif	// ZSPEC

// -- `USB_CDC displaces `UART0 --
//
// Outside the ZSPEC guard above, deliberately: this applies to a
// release target composed by release/lib/gen.py exactly as it does to
// a hand-edited board block, and a target that adds `USB_CDC without
// remembering to write `-UART0` should get the same machine either
// way rather than a subtly different one.
//
// The two are alternatives rather than additions. They answer the
// same address window (0xf000_00xx, cs_uart0 in rtl/sysctl.v), and
// only one thing can. Building both would also declare UART0_TX with
// nothing driving it, which is not a harmless dangling net: it
// synthesises to a pin held at a constant, so a PMOD plugged into
// that connector would see a dead line rather than no line -- the
// same failure the `GPU_COMPOSITE port guards in rtl/sysctl.v exist
// to avoid.
//
// Done here, once, rather than by asking each board block to comment
// out `UART0 next to its `USB_CDC. Two defines that must always
// disagree are a rule, and a rule belongs in one place; the
// alternative is a board that has both because somebody uncommented
// one line and not the other, and the resulting build is a placement
// conflict several minutes into nextpnr rather than an obvious
// mistake.
//
// Note the direction. `USB_CDC wins because it is the deliberate,
// newly-added thing and `UART0 is on nearly every board by default --
// so a board gains a USB console by adding one line, which is the
// change somebody actually wants to make.
`ifdef USB_CDC
`undef UART0
`endif

`endif
