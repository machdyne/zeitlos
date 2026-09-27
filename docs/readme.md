# Zeitlos documentation

The book is **[Timeless Computing](tc.md)**: an introduction to
computing, FPGAs and the idea of a timeless computer, and the frame for
everything below.

The rest of this directory documents the implementation, one file per
subsystem or app, grouped below by subject. To use the system, start
with [welcome](welcome.md); to program it, [kernel](kernel.md) and
[app runtime](app_runtime.md).

## Getting started

| Document | Subject |
|---|---|
| [welcome](welcome.md) | First steps on the desktop, hotkeys |
| [toolchain](toolchain.md) | FPGA tools and the RISC-V compiler |
| [build](build.md) | How apps are built: `sw/common/app.mk` |
| [boards](boards.md) | Utilisation and timing per board |
| [ulx3s](ulx3s.md) | Radiona ULX3S notes |
| [releases](releases.md) | Prebuilt images and `release/zrelease` |
| [dfu\_upgrade](dfu_upgrade.md) | Upgrading the DFU bootloader |
| [config](config.md) | `/zeitlos.cfg` |

## SOC: CPU, memory and bus

| Document | Subject |
|---|---|
| [hwmap](hwmap.md) | The hardware map, drawn from RTL |
| [csrs](csrs.md) | Capability CSRs |
| [socctl](socctl.md) | SOC control register |
| [zeitlos32](zeitlos32.md) | The in-house RV32IM core |
| [muldiv](muldiv.md) | Hardware multiply and divide |
| [icache](icache.md) | Instruction cache |
| [dcache](dcache.md) | Unified I+D cache, write buffer, SDRAM bursts |
| [mpu](mpu.md) | Memory protection and crash reports |
| [ddr3](ddr3.md) | DDR3 main memory (Mozart ML2) |
| [ddr3-design](ddr3-design.md) | DDR3 datapath, second attempt |
| [ddr3-bringup](ddr3-bringup.md) | DDR3 bring-up log |
| [ddr3-status](ddr3-status.md) | DDR3 status log |
| [spiflash](spiflash.md) | The flash controller |
| [zboot](zboot.md) | Multiboot, PROGRAMN and the flash layout |
| [boot](boot.md) | Boot sequence and memory budget |
| [probe](probe.md) | Built-in logic probe |
| [trng](trng.md) | Ring-oscillator TRNG |
| [montmul](montmul.md) | Montgomery multiplier |
| [crypto\_hw\_options](crypto_hw_options.md) | Crypto acceleration options |
| [crypto\_perf](crypto_perf.md) | Where TLS time goes |
| [rtc](rtc.md) | RTC and time sync |

## SOC: video, audio and I/O

| Document | Subject |
|---|---|
| [gpu\_blitter](gpu_blitter.md) | Blitter developer guide |
| [gpu\_raster](gpu_raster.md) | Line rasterizer developer guide |
| [game\_mode](game_mode.md) | 320x240 viewport |
| [composite](composite.md) | Composite NTSC/PAL output |
| [audio](audio.md) | Hardware mixer and outputs |
| [uart](uart.md) | UART0 |
| [uart1](uart1.md) | Second UART and the `serial` service |
| [usb\_cdc](usb_cdc.md) | USB CDC-ACM console |
| [usb\_host](usb_host.md) | USB host controller |
| [usb\_ethernet](usb_ethernet.md) | USB CDC-ECM ethernet |
| [gamepad](gamepad.md) | Gamepads |
| [gpio](gpio.md) | GPIO on PMOD ports |
| [i2c](i2c.md) | Bit-banged I2C |
| [spi](spi.md) | SPI |
| [sdcard](sdcard.md) | SD card performance |
| [esp32link](esp32link.md) | ESP32 network link |
| [katze](katze.md) | Katze RMII ethernet PMOD |

## Kernel and OS services

| Document | Subject |
|---|---|
| [kernel](kernel.md) | Processes, scheduler, memory, syscalls |
| [app\_runtime](app_runtime.md) | The app side of the syscall boundary |
| [executables](executables.md) | The ZEXE format and loading |
| [flash\_apps](flash_apps.md) | Core apps in flash |
| [messaging](messaging.md) | Objects and mailboxes |
| [ports](ports.md) | Connections built on messaging |
| [connections](connections.md) | Connecting apps to services |
| [filesystem](filesystem.md) | FatFs, the syscall API and concurrency |
| [ramdisk](ramdisk.md) | The RAM disk (`/ram`) |
| [kvstore](kvstore.md) | Flash key/value store |
| [security](security.md) | Password and screen lock |
| [console](console.md) | The console service |
| [cron](cron.md) | Scheduled programs |
| [user\_input](user_input.md) | Keyboard and mouse |
| [keyboard\_layouts](keyboard_layouts.md) | 22 layouts and Japanese input |
| [line\_editing](line_editing.md) | Shared line editor |
| [text\_encoding](text_encoding.md) | UTF-8, Latin-9 and the fonts |
| [tts](tts.md) | Speech |
| [tts\_data](tts_data.md) | Speech data: `tools/speech` |
| [captions](captions.md) | System-wide captions |

## Graphics and the desktop

| Document | Subject |
|---|---|
| [window\_manager](window_manager.md) | `wm` developer guide |
| [widgets](widgets.md) | Widgets and dialogs |
| [libz](libz.md) | Runtime for zcc-compiled programs |
| [png](png.md) | PNG decoding |
| [svg](svg.md) | SVG rendering |
| [remote\_desktop](remote_desktop.md) | Remote desktop in a browser |

## Networking

| Document | Subject |
|---|---|
| [networking](networking.md) | The `net` service: IP, TCP, drivers |
| [netserve](netserve.md) | SSH, telnet and HTTP servers |
| [ssh](ssh.md) | SSH client and server |
| [tls](tls.md) | TLS 1.3 |
| [tls\_resumption](tls_resumption.md) | Keep-alive and session resumption |
| [x509](x509.md) | Certificate parsing |
| [http](http.md) | HTTP and the fetch service |
| [html\_layout](html_layout.md) | HTML parsing and layout |
| [gopher\_gemini](gopher_gemini.md) | Gopher and Gemini in `web` |

## Shells and languages

| Document | Subject |
|---|---|
| [terminal](terminal.md) | `term` |
| [posix](posix.md) | POSIX userland |
| [scheme](scheme.md) | Scheme in `repl` |
| [scheme\_api](scheme_api.md) | The Scheme API |
| [editor](editor.md) | `te` in `repl` |
| [zcc](zcc.md) | The C compiler |
| [zcc\_bringup](zcc_bringup.md) | Testing zcc on hardware |
| [zfpga](zfpga.md) | FPGA toolchain on the machine itself |
| [zfpga-formats](zfpga-formats.md) | zfpga file formats |
| [zfpga-test](zfpga-test.md) | zfpga's first board test |

## Apps

| Document | Subject |
|---|---|
| [file\_browser](file_browser.md) | `files` |
| [text\_editor](text_editor.md) | `text` |
| [sheet\_app](sheet_app.md) | `sheet` |
| [web\_app](web_app.md) | `web` |
| [read\_app](read_app.md) | `read` |
| [hex\_editor](hex_editor.md) | `hex` |
| [view\_app](view_app.md) | `view` |
| [calc\_app](calc_app.md) | `calc` |
| [info\_app](info_app.md) | `info` |
| [clock\_app](clock_app.md) | `clock` |
| [cal\_app](cal_app.md) | `cal` |
| [settings\_app](settings_app.md) | `settings` |
| [keyboard\_app](keyboard_app.md) | `keyboard` |
| [ask\_app](ask_app.md) | `ask` |
| [irc\_app](irc_app.md) | `irc` |
| [automate](automate.md) | `automate` |
| [demo](demo.md) | The scripted demos |
| [play\_app](play_app.md) | `play` |
| [track\_app](track_app.md) | `track` |
| [midi\_app](midi_app.md) | `midi` |
| [mmod](mmod.md) | `mmod` |
| [mesh\_app](mesh_app.md) | `mesh` |
| [logic\_app](logic_app.md) | `logic` |
| [gpu3d\_app](gpu3d_app.md) | `gpu3d` |

## Games

| Document | Subject |
|---|---|
| [chess\_app](chess_app.md) | `chess` |
| [chess\_engine](chess_engine.md) | The chess engine |
| [chip8\_app](chip8_app.md) | `chip8` |
| [gamedemo](gamedemo.md) | `gamedemo` |
| [kidgames\_app](kidgames_app.md) | `kidgames` |
| [casino](casino.md) | The casino apps, overview |
| [casino\_app](casino_app.md) | `casino`, the front desk |
| [casino\_bank](casino_bank.md) | Shared casino code |
| [blackjack](blackjack.md) | `blackjack` |
| [craps](craps.md) | `craps` |
| [poker\_app](poker_app.md) | `poker`, the table |
| [poker\_engine](poker_engine.md) | `poker`, the engine |
| [roulette](roulette.md) | `roulette` |
| [slots](slots.md) | `slots` |
