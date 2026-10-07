// Built-in logic analyser -- rtl/probe.v, docs/probe.md.
//
// Define `PROBE on a board (or pass -DPROBE) to include it. These size
// its capture buffer: 512 words is 8192 samples of two wires, one
// DP16KD, and 170 us at 48 MHz -- longer than a whole low-speed USB
// transaction. PROBE_AW must be log2(PROBE_WORDS).
//
// It is a bring-up instrument and off by default everywhere.
`ifdef PROBE
`ifndef PROBE_WORDS
`define PROBE_WORDS 512
`define PROBE_AW 9
`endif
`endif

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

// Memory protection unit: rtl/mpu.v, docs/mpu.md. A stray store or jump
// in an app ends that app with a crash report instead of corrupting
// the kernel, another app, or hardware state (flash, the SD card, the
// FPGA reconfigure key).
//
// Universal for the same reason `RTC and `TRNG are: no pins, no external
// part, no block RAM, and no board-specific behaviour. It also works
// with any cache arrangement, including none (Obst), and with either
// CPU. It costs no cycles (it checks in parallel with the cache lookup
// and never delays a permitted access); it costs roughly 450-700 logic
// cells, which matters only on the 25F boards -- see docs/mpu.md for
// the timing measured with it. The kernel programs it at boot and
// enforces by default (`mpu report` in the shell logs instead); a
// kernel that does not know about it leaves it disabled, which passes
// everything through unchanged.
`define MPU

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

// The virtual (software-written) mouse at 0xf000_0400: a 25-bit
// register {present, buttons, y, x} that, while present is set, drives
// the hardware cursor sprite and is read by wm exactly like a USB
// mouse. It began as the ULX3S remote desktop's pointer (sw/apps/net,
// docs/esp32link.md) and is universal now because scripted demos
// (sw/apps/automate, docs/automate.md) need to move the pointer on
// every board. About 25 flip-flops and a 10-bit two-way mux.
//
// Defining it narrows the UART0 decode to 0xf000_00xx (rtl/sysctl.v):
// without that, 0xf000_0400 would alias the console UART. Software
// checks Z_FEATURE2_VMOUSE (sw/common/zsoc.h) before writing it.
`define VMOUSE

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
// PROGRAMN is wired to user pin M8 (boards/*.lpf): rtl/socctl.v's
// RECONFIG, `reboot`, docs/zboot.md. Confirmed for Lakritz, Obst and
// Mozart ML1 and ML2; boards whose site is not confirmed do not define it.
`define PROGRAMN_PIN
`define OSC48
`define MEM 1				// note that some Obst boards have 2MB SRAM
`define MEM_SRAM
`define MEM_VRAM
//`define MEM_QQSPI
`define MEM_ROM
`define MONTMUL
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
// ON by default. The console is the USB-C socket that already has a
// cable in it, PMOD A is free for something else, and a board with no
// PMODs at all still shows you a prompt -- which is what a new owner
// has.
//
// `UART0 goes away with it: the `undef at the bottom of this file
// hands the 0xf000_00xx window to the USB device instead of to
// rtl/ext/uart16550. A second hardware UART is a custom build now,
// not a shipped target.
//
// This does NOT constrain PMOD A to anything on its own; it only
// stops the console needing it. The obst_langkatze_gpio release
// target is what uses the freed connector: Langkatze on PMOD A, GPIO
// on PMOD B (target names list PMODs in port order).
`define USB_CDC

// GPIO (rtl/gpio.v, docs/gpio.md) is OFF in the plain board build.
// boards/obst_v0.lpf still constrains the Langkatze ethernet PMOD on
// PMOD B (`SPI_ETH), and uncommenting the line below without moving
// it puts two top-level ports on the same balls.
//
// The supported way to build a GPIO Obst is the release system, which
// generates the constraints itself (see the ZSPEC note above):
//
//     ./release/zrelease build obst_langkatze_gpio
//
// That target puts the Langkatze on PMOD A and GPIO port 0 on PMOD B
// -- name order is port order -- with the console on the USB-C
// socket. Both PMOD ports are therefore taken by the target, and the
// base file's PMOD B ethernet constraints are released and
// regenerated on PMOD A.
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
// PROGRAMN is wired to user pin M8 (boards/*.lpf): rtl/socctl.v's
// RECONFIG, `reboot`, docs/zboot.md. Confirmed for Lakritz, Obst and
// Mozart ML1 and ML2; boards whose site is not confirmed do not define it.
`define PROGRAMN_PIN
`define OSC48
`define MEM 32
`define MEM_SDRAM
`define MEM_VRAM
`define MEM_ROM
`define MEM_GLYPH
`define ICACHE
// rtl/montmul.v -- Montgomery modular multiplier for TLS.
//
// ON FOR EVERY BOARD. Certificate verification was 36 of the 85
// seconds a page load took in sw/apps/web, and that is not a
// Lakritz-specific cost -- any board that talks TLS pays it.
//
// Costs a handful of DSP slices and ~50 words of distributed LUT RAM.
// NO BRAM. Software falls back to its own field arithmetic if it is
// absent, so dropping it on a board that stops fitting costs speed
// rather than function.
`define MONTMUL
// Its register file (docs/montmul.md), ~510 LUT4 and one DP16KD, and
// the SHA-256 block (docs/sha256_hw.md), ~1,000 LUT4-equivalents and
// one DP16KD. The tightest board: see docs/sha256_hw.md, "Fitting",
// for what these cost here.
`define MONTMUL_REGS
`define SHA256
`define ICACHE_KB 4
`define ICACHE_LINE_WORDS 4
// Data cache (docs/dcache.md), WITHOUT the write buffer: this 25F is
// the tightest board, and DCACHE_WBUF 0 gives back ~540 LUT4 for about
// 7% of the D-cache's gain. With the write buffer it measured 88% full
// and 49.9-54.6 MHz over three seeds -- passing, but only 4% over on
// the worst seed. See docs/dcache.md "Boards and releases" for the numbers
// with this setting.
`define DCACHE
`define DCACHE_KB 4
`define DCACHE_LINE_WORDS 4
`define DCACHE_WBUF 0
`define SDRAM_BURST
`define GPU
`define GPU_RASTER
`define GPU_BLIT
`define GPU_CURSOR
`define GPU_DDMI
`define UART0
`define USB_HOST
`define SPI_SDCARD
`define SPI_ETH
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

// Katze RMII ethernet PMOD instead of Langkatze: off in the plain
// board build, which assumes a Langkatze in port A. The switch is
// `SPI_ETH out and these two in, plus the commented Katze block in
// boards/lakritz_v0.lpf:
//
//     ./release/zrelease build lakritz_katze
//
// does all of it from one spec. See docs/katze.md.
//`define ETH_RMII
//`define ETH_RX_SLOTS 4

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
// PROGRAMN is wired to user pin M8 (boards/*.lpf): rtl/socctl.v's
// RECONFIG, `reboot`, docs/zboot.md. Confirmed for Lakritz, Obst and
// Mozart ML1 and ML2; boards whose site is not confirmed do not define it.
`define PROGRAMN_PIN
`define OSC48
`define MEM 32
`define MEM_SDRAM
`define MEM_VRAM
`define MEM_ROM
`define MEM_GLYPH
`define ICACHE
`define MONTMUL
`define ICACHE_KB 8
`define ICACHE_LINE_WORDS 4
// Data cache: rtl/cache_id.v's unified wb_cache replaces wb_icache
// (docs/dcache.md). Comment out DCACHE to get the I-cache-only build
// back exactly; comment out ICACHE as well for no cache at all.
// SDRAM_BURST makes line fills 4-word SDRAM bursts (rtl/mem/
// sdram_kianv.v BURST=1); independent of the rest, and the first
// thing to drop if bring-up misbehaves.
`define DCACHE
`define DCACHE_KB 4
`define DCACHE_LINE_WORDS 4
`define DCACHE_WBUF 2
`define SDRAM_BURST
`define GPU
`define GPU_RASTER
`define GPU_BLIT
`define GPU_CURSOR
`define GPU_DDMI
`define UART0
// USB host controller (rtl/usb/, docs/usb_host.md) instead of the
// low-speed HID-only rtl/ext/usb_hid_host. Both cores remain
// supported and USB_HID is still the default on every other board --
// see rtl/sysctl.v, where USB_HOST `undef's USB_HID because the two
// are two cores for the same two pins.
`define USB_HOST
`define SPI_SDCARD
`define ETH_RMII
// See the note on ETH_RX_SLOTS under BOARD_SERGEI_ML1.
`define ETH_RX_SLOTS 4
`define AUDIO
`define AUDIO_PT8211
`define AUDIO_MIXER

// Built-in logic analyser (rtl/probe.v). Watching USB host port 0's
// D+/D- by default -- see the probe instantiation in rtl/sysctl.v for
// what it is wired to and what triggers it.
//
// One DP16KD and ~100 LUT4. This is a 45F, so both are affordable;
// take it back out once USB bring-up is done.
//`define PROBE
//`define PROBE_WORDS 512
//`define PROBE_AW 9

`elsif BOARD_MOZART_ML2

`define FPGA_ECP5
// PROGRAMN on M8, as on ML1 (boards/mozart_ml2.lpf).
`define PROGRAMN_PIN
`define OSC48
`define MEM 512
// DDR3 (rtl/mem/ddr3*.v, docs/ddr3.md). The part decides two things,
// from its datasheet -- all three candidates are x16, 8 banks, 10
// column bits:
//
//   MT41K64M16TW-107:J    1Gb  128MB  13 row bits  tRFC 110ns
//   MT41K128M16JT-125:K   2Gb  256MB  14 row bits  tRFC 160ns
//   MT41K256M16TW-107:P   4Gb  512MB  15 row bits  tRFC 260ns
//
// tRFC MUST match the density: the 1Gb value on a 4Gb part issues
// commands to a DRAM still refreshing, and the corruption that
// follows is intermittent and looks like a tuning problem.
//
// ML2 carries the 4Gb part, and all 512MB of it is main memory:
// 0x4000_0000-0x5fff_ffff (spieth moved to 0x6100_0000 to make room).
//
// MAIN_512MB tells everything that asks "is this main memory" -- both
// caches, the MPU and the cache snoop -- that 0x5 counts, from one
// parameter set in rtl/sysctl.v. The caches' tags widen by a bit with
// it; without that, 0x4000_0000 and 0x5000_0000 would share lines. And
// without the MPU's part, stores above 0x5000_0000 would be gated only
// by MASK, which allows nibble 5 by default.
`define MEM_DDR3
`define MAIN_512MB
`define DDR3_ROW_BITS 15
`define DDR3_TRFC_NS 260
`define MEM_VRAM
`define MEM_ROM
`define MEM_GLYPH
`define ICACHE
`define MONTMUL
`define ICACHE_KB 8
`define ICACHE_LINE_WORDS 4
// Data cache: rtl/cache_id.v's unified wb_cache replaces wb_icache
// (docs/dcache.md). Comment out DCACHE to get the I-cache-only build
// back exactly; comment out ICACHE as well for no cache at all.
`define DCACHE
`define DCACHE_KB 4
`define DCACHE_LINE_WORDS 4
`define DCACHE_WBUF 2
`define GPU
`define GPU_RASTER
`define GPU_BLIT
`define GPU_CURSOR
`define GPU_DDMI
`define UART0
// USB host controller (rtl/usb/, docs/usb_host.md) instead of the
// low-speed HID-only rtl/ext/usb_hid_host. Both cores remain
// supported and USB_HID is still the default on every other board --
// see rtl/sysctl.v, where USB_HOST `undef's USB_HID because the two
// are two cores for the same two pins.
`define USB_HOST
`define SPI_SDCARD
`define ETH_RMII
// See the note on ETH_RX_SLOTS under BOARD_SERGEI_ML1.
`define ETH_RX_SLOTS 4
`define AUDIO
`define AUDIO_PT8211
`define AUDIO_MIXER

// Built-in logic analyser (rtl/probe.v). Watching USB host port 0's
// D+/D- by default -- see the probe instantiation in rtl/sysctl.v for
// what it is wired to and what triggers it.
//
// One DP16KD and ~100 LUT4. This is a 45F, so both are affordable;
// take it back out once USB bring-up is done.
//`define PROBE
//`define PROBE_WORDS 512
//`define PROBE_AW 9

`elsif BOARD_NOIR

// Machdyne Noir: ECP5 45F, 256MB DDR3L, DDMI, one USB host port,
// microSD, 3.5mm audio. No PMOD, no ethernet. Pins: boards/noir_v0.lpf.
//
// The DDR3 is on the Sechzig ML2's balls, so the memory half of this
// block is Mozart ML2's (docs/ddr3.md) with the part changed: Noir
// carries the MT41K128M16JT-125:K, 2Gb, so 14 row bits and the 2Gb
// tRFC. NOT `MAIN_512MB: with a 256MB part the DDR3 decode covers 0x4
// only and 0x5 stays empty, which docs/ddr3.md ("Smaller parts")
// explains is required, not merely tidy.
//
// The rest is a desktop 45F: Schoko's cache and crypto settings, the
// full-speed USB host, and sigma-delta audio with the hardware mixer.
`define FPGA_ECP5
// PROGRAMN on M8 (the schematic's RESET net), as on every Machdyne
// board with tinydfu-bootloader.
`define PROGRAMN_PIN
`define OSC48
`define MEM 256
`define MEM_DDR3
`define DDR3_ROW_BITS 14
`define DDR3_TRFC_NS 160
`define MEM_VRAM
`define MEM_ROM
`define MEM_GLYPH
`define LED_RGB
`define ICACHE
`define MONTMUL
`define MONTMUL_REGS
`define SHA256
`define ICACHE_KB 8
`define ICACHE_LINE_WORDS 4
`define DCACHE
`define DCACHE_KB 4
`define DCACHE_LINE_WORDS 4
`define DCACHE_WBUF 2
`define GPU
`define GPU_RASTER
`define GPU_BLIT
`define GPU_CURSOR
`define GPU_DDMI
`define UART0
// The console is the USB-C socket (`undef at the bottom drops
// `UART0): Noir has no PMOD, so this is the console that needs no
// soldering. The debug UART is a pair of pads, J1/J2.
`define USB_CDC
`define USB_HOST
`define SPI_SDCARD
`define AUDIO
`define AUDIO_SD
`define AUDIO_MIXER

`elsif BOARD_KLINGE

// Machdyne Klinge: ECP5 25F, 512MB DDR3L, two LAN8720A RMII PHYs, two
// microSD slots, USB-C. Headless. Pins: boards/klinge_v1.lpf. Only
// the first PHY and the first slot are used.
//
// MEMORY. The DDR3 is on the Sechzig ML2's balls with the ML2's 4Gb
// part, so these four lines are Mozart ML2's, all 512MB of it.
//
// HEADLESS, BUT WITH A FRAMEBUFFER. No `GPU: there is no connector to
// scan out to, and no pll1 (rtl/sysctl.v builds it only with `GPU),
// which is what lets this fit the 25F's two PLLs beside the DDR3 one.
// `MEM_VRAM, `GPU_RASTER and `GPU_BLIT stay: the 640x480 framebuffer
// and the line and blit engines all run on sys_clk and need no video
// clock, so software that draws -- the panic screen, and a future
// remote desktop over ethernet -- draws at hardware speed into a
// screen nobody is plugged into.
//
// CONSOLE. USB CDC-ACM on the USB-C socket; the `undef at the bottom
// drops `UART0. Klinge's UART is a pair of pads (R6/R7).
//
// NETWORK. The RMII MAC with the FPGA driving the PHYs' 50MHz
// reference (pll0 -> T4), as Sergei ML1 does.
`define FPGA_ECP5
// PROGRAMN on M8 (the schematic's SYS_RST_N, tinydfu's resetn).
`define PROGRAMN_PIN
`define OSC48
`define MEM 512
`define MEM_DDR3
`define MAIN_512MB
`define DDR3_ROW_BITS 15
`define DDR3_TRFC_NS 260
`define MEM_VRAM
`define MEM_ROM
`define MEM_GLYPH
`define ICACHE
`define MONTMUL
`define MONTMUL_REGS
`define SHA256
`define ICACHE_KB 4
`define ICACHE_LINE_WORDS 4
`define DCACHE
`define DCACHE_KB 4
`define DCACHE_LINE_WORDS 4
`define DCACHE_WBUF 2
`define GPU_RASTER
`define GPU_BLIT
`define UART0
`define USB_CDC
`define SPI_SDCARD
`define ETH_RMII
`define ETH_RMII_DRIVE_REFCLK
`define ETH_RX_SLOTS 4

`elsif BOARD_SERGEI_ML1

`define FPGA_ECP5
// PROGRAMN is wired to user pin M8 (boards/*.lpf): rtl/socctl.v's
// RECONFIG, `reboot`, docs/zboot.md. Sergei ML1 carries the same ML1
// module as Mozart ML1 -- clock, flash and SDRAM pins are identical --
// so M8 is taken from Mozart ML1's confirmed wiring.
`define PROGRAMN_PIN
`define OSC48
`define MEM 32
`define MEM_SDRAM
`define MEM_VRAM
`define MEM_ROM
`define MEM_GLYPH
`define ICACHE
`define MONTMUL
`define ICACHE_KB 8
`define ICACHE_LINE_WORDS 4
// Data cache (docs/dcache.md). Same part and SDRAM as mozart_ml1,
// where it was brought up. Measured: 44% -> 48% of logic cells,
// 42 -> 45 of 108 DP16KD, 53.3 -> 54.5 MHz (one seed).
`define DCACHE
`define DCACHE_KB 4
`define DCACHE_LINE_WORDS 4
`define DCACHE_WBUF 2
`define SDRAM_BURST
`define GPU
`define GPU_RASTER
`define GPU_BLIT
`define GPU_CURSOR
`define GPU_DDMI
`define UART0
`define USB_HOST
`define SPI_SDCARD
`define ETH_RMII
`define ETH_RMII_DRIVE_REFCLK
// Frames the RMII receive FIFO holds, one full-size frame per slot.
// Power of two, 2 or more. 2KB of block RAM each, one DP16KD per slot.
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

`elsif BOARD_SERGEI_MX1

// Sechzig MX1 (Artix-7 XC7A35T, 32 MB SDRAM, 4 MB flash) in a Sergei
// carrier: the Sergei ML1 board with an Artix-7 module. Pins:
// boards/sergei_mx1.xdc.
`define FPGA_XC7
`define OSC48

// Zeitlos's 1 MB of flash starts at 3 MB (docs/boot.md): the
// bitstream is a fixed 2.09 MB on this die. Reported to software by
// rtl/csrs.v word 63. KEEP IN SYNC with FLASH_BASE in the Makefile.
`define FLASH_BASE 32'h0030_0000
// No software write below the end of the bitstream (rtl/spiflash.v).
`define SPIFLASH_LOCK_END 24'h220000

// Workarounds for the open Artix-7 toolchain, both found on Kirsch
// (docs/toolchain.md): the TRNG's ring oscillators do not route, and
// the DSP48E1 behind the fast multiplier hangs the CPU's PCPI
// handshake. Multiplication stays in hardware -- `CPU_MUL is the
// sequential multiplier -- so software is unchanged.
`define NO_TRNG
`define NO_CPU_MUL_FAST

// Bring-up: the board LED shows how far the SOC got (rtl/sysctl.v).
`define BRINGUP_LED_STATUS

// The SDRAM clock 90 degrees behind sys_clk, both from one MMCM, with
// rising-edge read capture -- LiteX's arrangement (rtl/sysctl.v).
`define SDRAM_CLK90

`define MEM 32
`define MEM_SDRAM
`define MEM_VRAM
`define MEM_ROM
`define MEM_GLYPH

// First bring-up: no instruction or data cache, and no MONTMUL (it
// multiplies, i.e. DSP48E1s). Each is one line to turn back on once
// the board boots -- the ML1 module has ICACHE_KB 8 / DCACHE_KB 4 /
// DCACHE_WBUF 2 / SDRAM_BURST / MONTMUL.
//`define ICACHE
//`define ICACHE_KB 8
//`define ICACHE_LINE_WORDS 4
//`define DCACHE
//`define DCACHE_KB 4
//`define DCACHE_LINE_WORDS 4
//`define DCACHE_WBUF 2
//`define SDRAM_BURST
//`define MONTMUL

`define GPU
`define GPU_RASTER
`define GPU_BLIT
`define GPU_CURSOR
`define GPU_DDMI
// The console: UART0 on the module's UART_TX/UART_RX (L2/L3), which
// the carrier's RP2040 bridges to USB -- the same dirtyJtag that
// programs the board. (Not XC/XD: those reach the RP2040 too, unused.)
`define UART0
`define USB_HOST
`define SPI_SDCARD
`define ETH_RMII
`define ETH_RX_SLOTS 4
`define AUDIO
`define AUDIO_MIXER
`define ETH_RMII_DRIVE_REFCLK
`define AUDIO_SPDIF
`define AUDIO_RATE_RESET 8'd16

`elsif BOARD_MOZART_MX1

// The same module in a Mozart carrier. Pins: boards/mozart_mx1.xdc.
`define FPGA_XC7
`define OSC48

// Zeitlos's 1 MB of flash starts at 3 MB (docs/boot.md): the
// bitstream is a fixed 2.09 MB on this die. Reported to software by
// rtl/csrs.v word 63. KEEP IN SYNC with FLASH_BASE in the Makefile.
`define FLASH_BASE 32'h0030_0000
// No software write below the end of the bitstream (rtl/spiflash.v).
`define SPIFLASH_LOCK_END 24'h220000

// Workarounds for the open Artix-7 toolchain, both found on Kirsch
// (docs/toolchain.md): the TRNG's ring oscillators do not route, and
// the DSP48E1 behind the fast multiplier hangs the CPU's PCPI
// handshake. Multiplication stays in hardware -- `CPU_MUL is the
// sequential multiplier -- so software is unchanged.
`define NO_TRNG
`define NO_CPU_MUL_FAST

// Bring-up: the board LED shows how far the SOC got (rtl/sysctl.v).
`define BRINGUP_LED_STATUS

// The SDRAM clock 90 degrees behind sys_clk, both from one MMCM, with
// rising-edge read capture -- LiteX's arrangement (rtl/sysctl.v).
`define SDRAM_CLK90

`define MEM 32
`define MEM_SDRAM
`define MEM_VRAM
`define MEM_ROM
`define MEM_GLYPH

// First bring-up: no instruction or data cache, and no MONTMUL (it
// multiplies, i.e. DSP48E1s). Each is one line to turn back on once
// the board boots -- the ML1 module has ICACHE_KB 8 / DCACHE_KB 4 /
// DCACHE_WBUF 2 / SDRAM_BURST / MONTMUL.
//`define ICACHE
//`define ICACHE_KB 8
//`define ICACHE_LINE_WORDS 4
//`define DCACHE
//`define DCACHE_KB 4
//`define DCACHE_LINE_WORDS 4
//`define DCACHE_WBUF 2
//`define SDRAM_BURST
//`define MONTMUL

`define GPU
`define GPU_RASTER
`define GPU_BLIT
`define GPU_CURSOR
`define GPU_DDMI
// The console: UART0 on the module's UART_TX/UART_RX (L2/L3), which
// the carrier's RP2040 bridges to USB -- the same dirtyJtag that
// programs the board. (Not XC/XD: those reach the RP2040 too, unused.)
`define UART0
`define USB_HOST
`define SPI_SDCARD
`define ETH_RMII
`define ETH_RX_SLOTS 4
`define AUDIO
`define AUDIO_MIXER
`define AUDIO_PT8211

`elsif BOARD_SERGEI_ML2

// Sergei with the Sechzig ML2 module: Sergei ML1's carrier choices, with
// the ML2 module's -- which are Mozart ML2's. Mozart ML1 -> ML2 changed
// exactly the memory defines and SDRAM_BURST; this block is
// BOARD_SERGEI_ML1 with that same change. See boards/sergei_ml2.lpf for
// why the PIN file is NOT a copy of sergei_ml1.lpf.

`define FPGA_ECP5
// PROGRAMN is wired to user pin M8 on the ML2 module, confirmed on
// Mozart ML2 (docs/zboot.md).
`define PROGRAMN_PIN
`define OSC48
// DDR3 main memory (docs/ddr3.md). These values are for the 4Gb part
// Mozart ML2 carries, MT41K256M16TW-107. The pin file allows 128MB to
// 512MB parts: a smaller one needs MEM, DDR3_ROW_BITS and DDR3_TRFC_NS
// from the table in docs/ddr3.md, and NO MAIN_512MB -- with it, the OS
// would use aliased memory above the part's size.
`define MEM 512
`define MEM_DDR3
`define MAIN_512MB
`define DDR3_ROW_BITS 15
`define DDR3_TRFC_NS 260
`define MEM_VRAM
`define MEM_ROM
`define MEM_GLYPH
`define ICACHE
`define MONTMUL
// Its register file and the SHA-256 block (docs/montmul.md,
// docs/sha256_hw.md): measured here, 52.25 MHz against 48, and
// cryptobench's known answers correct through both.
`define MONTMUL_REGS
`define SHA256
// The Keccak-f[1600] block (docs/keccak_hw.md): ~6,000 LUT4 and ~1,700
// FF -- room here, not on Lakritz. Optional like the others: take this
// line out and sw/common/zkeccak.c uses software.
`define KECCAK
`define ICACHE_KB 8
`define ICACHE_LINE_WORDS 4
`define DCACHE
`define DCACHE_KB 4
`define DCACHE_LINE_WORDS 4
`define DCACHE_WBUF 2
// No SDRAM_BURST: that selects burst line fills for the SDRAM
// controller; DDR3's answers a line fill from its own block register.
`define GPU
`define GPU_RASTER
`define GPU_BLIT
`define GPU_CURSOR
`define GPU_DDMI
`define UART0
`define USB_HOST
`define SPI_SDCARD
`define ETH_RMII
`define ETH_RMII_DRIVE_REFCLK
// See the note on ETH_RX_SLOTS under BOARD_SERGEI_ML1.
`define ETH_RX_SLOTS 4
`define AUDIO
`define AUDIO_SPDIF
`define AUDIO_MIXER
// 46875Hz, as BOARD_SERGEI_ML1: the S/PDIF half-cell in whole sys_clks.
`define AUDIO_RATE_RESET 8'd16

// GPIO on the 6-pin PMOD, four pins, off by default -- the same trade
// as BOARD_SERGEI_ML1 (pin 1 is the optical S/PDIF output), on the
// ML2 module's balls. See boards/sergei_ml2.lpf, whose ball map is
// deduced rather than measured.
//`define GPIO_PORT0
//`define GPIO_PORT0_NARROW

`elsif BOARD_LEBKUCHEN

`define FPGA_GATEMATE
`define OSC48
`define MEM 8
//`define MEM_QQSPI
//`define MEM_QQSPI_SINGLE
`define MEM_VRAM
`define MONTMUL
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
`define MONTMUL
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
`define MONTMUL
// Its register file and the SHA-256 block (docs/montmul.md,
// docs/sha256_hw.md): measured here, 51.15 MHz against 48,
// +1,383 COMB, +1,129 FF, +2 DP16KD. cryptobench's known
// answers correct through both. TLS to example.com from
// 9.1-9.5 s down to 3.5-3.6 s.
`define MONTMUL_REGS
`define SHA256
`define ICACHE_KB 4
`define ICACHE_LINE_WORDS 4
// Data cache (docs/dcache.md). Measured on the 25F die (which the 12F
// also is): 79% -> 86% of logic cells, 39 -> 42 of 56 DP16KD, 55.7,
// 57.5, 58.1 MHz over three seeds.
`define DCACHE
`define DCACHE_KB 4
`define DCACHE_LINE_WORDS 4
`define DCACHE_WBUF 2
`define SDRAM_BURST
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

// Receive FIFO depth, as a power of two. 13 = 8192 bytes.
//
// rtl/esp32_rxfifo.v buffers the ESP32's UART in block RAM because a
// 16550's 16-byte FIFO cannot hold a frame while a time-sliced
// process is away from the CPU -- about 4ms, or 1200 bytes at
// 3 Mbaud. 8K also leaves room for a credit burst of ~5 MTU frames.
//
// A DEFINE rather than a literal at the instantiation because the
// 12F does not have the block RAM for it: at 13 this costs four
// DP16KD out of 56, and the 12F build needs three of them back.
// 11 (2048 bytes) still covers the time-slice case with margin and
// gives up only the multi-frame burst headroom, which costs
// throughput under sustained load rather than dropping bytes.
//
// Overridden per target -- see release/targets/ulx3s_12f.spec. The
// 45F and 85F have the room and stay at 13.
`define ESP32_RXFIFO_BITS 13

`define AUDIO
`define AUDIO_SPDIF
`define AUDIO_MIXER
// 46875Hz -- see the `AUDIO_SPDIF note above. ULX3S runs from a 25MHz
// crystal rather than 48, but pll0_25 produces sys_clk of EXACTLY
// 48.0000 MHz (480MHz VCO / 10), so the S/PDIF arithmetic is identical
// to the 48MHz-crystal boards and the half-cell is still 8 cycles.
`define AUDIO_RATE_RESET 8'd16

`elsif BOARD_KONFEKT

// Machdyne Konfekt: ECP5 12F, 32MB SDRAM, DDMI, one USB host port,
// microSD, 3.5mm audio, no PMOD. Pins: boards/konfekt_v0.lpf.
//
// Electrically this is a Lakritz without the PMOD connector, on the
// 12F rather than the 25F -- which to the open-source tools is the
// same die with the same 24288 LUT4s and 56 DP16KD (docs/boards.md).
// So the block is Lakritz's, less `SPI_ETH (there is nowhere to plug a
// Langkatze in), plus the RGB LED. Lakritz's own budget notes apply
// unchanged: the D-cache is built without its write buffer, and
// `AUDIO_MIXER is the first thing to turn off if this stops fitting.
`define FPGA_ECP5
// PROGRAMN on M8: the net tinydfu-bootloader's konfekt_v0.lpf calls
// resetn and pulls the same way to leave the bootloader. By inference
// from that and from Lakritz and Obst, which share the arrangement.
`define PROGRAMN_PIN
`define OSC48
`define MEM 32
`define MEM_SDRAM
`define MEM_VRAM
`define MEM_ROM
`define MEM_GLYPH
`define LED_RGB
`define ICACHE
`define MONTMUL
`define MONTMUL_REGS
`define SHA256
`define ICACHE_KB 4
`define ICACHE_LINE_WORDS 4
`define DCACHE
`define DCACHE_KB 4
`define DCACHE_LINE_WORDS 4
`define DCACHE_WBUF 0
`define SDRAM_BURST
`define GPU
`define GPU_RASTER
`define GPU_BLIT
`define GPU_CURSOR
`define GPU_DDMI
`define UART0
// The console is the USB-C socket; the `undef at the bottom of this
// file drops `UART0. Konfekt has no PMOD, so this is the only console
// that needs no soldering (the debug UART is a pair of pads, J1/J2).
`define USB_CDC
`define USB_HID
`define SPI_SDCARD
`define AUDIO
`define AUDIO_SD
// No `AUDIO_MIXER. Lakritz, the same die with this block plus
// `SPI_ETH, measured 23395 of 24288 TRELLIS_COMB (96%) at the commit
// this port started from, and routed only after more than twenty
// minutes of congestion -- well past the ~75% where docs/boards.md
// says timing turns seed-sensitive. The hardware mixer is the largest
// optional block that costs nothing but speed to drop (docs/audio.md:
// sw/apps/mod detects it and mixes in software), so it goes first.
//`define AUDIO_MIXER

`elsif BOARD_MINZE

// Machdyne Minze: ECP5 12F, 32MB SDRAM, VGA, one USB host port,
// microSD, one PMOD, USB-C. No audio. Pins: boards/minze_v1.lpf.
//
// Konfekt's block on the same die, with VGA in place of DDMI and GPIO
// on the PMOD in place of audio. The console is USB CDC-ACM on the
// USB-C socket, so the PMOD is free: this block -- like the .lpf --
// describes the minze_gpio release target exactly.
//
// VGA here is one bit per colour on the MSB of each of the board's
// 3-bit resistor ladders (8 of its 512 colours).
`define FPGA_ECP5
// PROGRAMN on M8 (tinydfu-bootloader's resetn), as on Konfekt.
`define PROGRAMN_PIN
`define OSC48
`define MEM 32
`define MEM_SDRAM
`define MEM_VRAM
`define MEM_ROM
`define MEM_GLYPH
`define ICACHE
`define MONTMUL
`define MONTMUL_REGS
`define SHA256
`define ICACHE_KB 4
`define ICACHE_LINE_WORDS 4
`define DCACHE
`define DCACHE_KB 4
`define DCACHE_LINE_WORDS 4
`define DCACHE_WBUF 0
`define SDRAM_BURST
`define GPU
`define GPU_RASTER
`define GPU_BLIT
`define GPU_CURSOR
`define GPU_VGA
`define UART0
`define USB_CDC
`define USB_HID
`define SPI_SDCARD
`define GPIO_PORT0

`elsif BOARD_SCHOKO

// Machdyne Schoko: ECP5 45F, 32MB SDRAM, DDMI and VGA, one USB host
// port, microSD, two PMOD ports, no audio. Pins: boards/schoko_v1.lpf.
//
// A 45F with SDRAM, so its cache and crypto settings are Mozart ML1's
// plus the MONTMUL register file and the SHA-256 block, which the 45F
// has room for. The USB host is the full-speed controller (`USB_HOST)
// rather than the low-speed HID core, as on the other 45F boards, so
// a USB stick works in the front socket.
//
// THE PMODS. The console is USB CDC-ACM on the USB-C socket, so both
// PMOD ports are free, and this block -- like boards/schoko_v1.lpf --
// describes the schoko_langkatze_gpio release target exactly:
//
//   PMOD A   Langkatze ENC28J60 ethernet   `SPI_ETH
//   PMOD B   GPIO port 0, eight pins       `GPIO_PORT0
//
// release/targets/schoko_langkatze_gpio.spec plugs the same two in,
// and `zrelease check` reports its constraints as identical to the
// board file's.
`define FPGA_ECP5
// PROGRAMN on M8, as Konfekt (tinydfu-bootloader's resetn).
`define PROGRAMN_PIN
`define OSC48
`define MEM 32
`define MEM_SDRAM
`define MEM_VRAM
`define MEM_ROM
`define MEM_GLYPH
`define LED_RGB
`define ICACHE
`define MONTMUL
`define MONTMUL_REGS
`define SHA256
`define ICACHE_KB 8
`define ICACHE_LINE_WORDS 4
`define DCACHE
`define DCACHE_KB 4
`define DCACHE_LINE_WORDS 4
`define DCACHE_WBUF 2
`define SDRAM_BURST
`define GPU
`define GPU_RASTER
`define GPU_BLIT
`define GPU_CURSOR
// Both video outputs, from one timing generator: they show the same
// 640x480 picture (rtl/sysctl.v drives VGA and DDMI side by side).
`define GPU_VGA
`define GPU_DDMI
`define UART0
`define USB_CDC
`define USB_HOST
`define SPI_SDCARD
`define SPI_ETH
`define GPIO_PORT0

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

// A board opting OUT of a universal feature, for a toolchain that
// cannot yet build it (the Artix-7 boards: see their blocks). Software
// copes as it does on any board without the feature -- the FEATURES
// bit is clear -- so nothing else changes. A board that sets one of
// these has an open bug.
`ifdef NO_TRNG
`undef TRNG
`endif
`ifdef NO_CPU_MUL_FAST
`undef CPU_MUL_FAST
`endif

`endif
