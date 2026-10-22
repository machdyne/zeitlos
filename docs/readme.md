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
| [config](config.md) | `/sys/zeitlos.cfg` |

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
| [ddr3](ddr3.md) | DDR3 main memory (Mozart ML2, Sergei ML2) |
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
| [color](color.md) | Game-mode colour: bitplanes and a 16-entry palette |
| [composite](composite.md) | Composite NTSC/PAL output |
| [audio](audio.md) | Hardware mixer and outputs |
| [ddmi_audio](ddmi_audio.md) | Sound and display info over the DDMI connector (in progress) |
| [uart](uart.md) | UART0 |
| [uart1](uart1.md) | Second UART and the `serial` service |
| [usb\_cdc](usb_cdc.md) | USB CDC-ACM console |
| [usb\_host](usb_host.md) | USB host controller |
| [usb\_ethernet](usb_ethernet.md) | USB CDC-ECM ethernet |
| [gamepad](gamepad.md) | Gamepads |
| [gpio](gpio.md) | GPIO on PMOD ports, stream engines (zlink, SPI, UART, raw on any pin), and the optional I2C slave |
| [zlink](zlink.md) | Linking two machines over a couple of wires: tiers, wiring, speeds, and experimental fast zlink over Mozart's DS port |
| [zlink\_app](zlink_app.md) | `zlink`: files and a shell between two machines over that link |
| [i2c](i2c.md) | Bit-banged I2C |
| [sechs\_app](sechs_app.md) | `sechs`: Sechs modules on a PMOD |
| [i2c\_app](i2c_app.md) | `i2c`: I2C from the shell, on a PMOD or a bench bus |
| [bench](bench.md) | `bench`: virtual parts on I2C buses, from netlists |
| [ls99](ls99.md) | `ls99`: virtual Zwölf modules running BASIC, on the bench |
| [spi](spi.md) | SPI |
| [sdcard](sdcard.md) | SD card performance |
| [esp32link](esp32link.md) | ESP32 network link |
| [kabellos](kabellos.md) | Kabellos ESP32-C3 WiFi PMOD: target, firmware, flashing |
| [katze](katze.md) | Katze RMII ethernet PMOD |

## Kernel and OS services

| Document | Subject |
|---|---|
| [kernel](kernel.md) | Processes, scheduler, memory, syscalls |
| [userspace\_drivers](userspace_drivers.md) | Drivers as processes: status, how they start and restart, open issues, then the design and the log |
| [fs\_driver](fs_driver.md) | The filesystem as a driver: `fs`, its protocol and client |
| [usb\_driver](usb_driver.md) | USB as a driver: `usb`, its protocol, the ethernet rings, drivers past the flash |
| [posix\_io](posix_io.md) | libc `open()`/`fopen()` over `fs`, for apps built with `POSIX_IO = 1` |
| [net\_throughput](net_throughput.md) | Network throughput: where the time goes, and the plan past 50KB/s |
| [app\_runtime](app_runtime.md) | The app side of the syscall boundary |
| [executables](executables.md) | The ZEXE format and loading |
| [flash\_apps](flash_apps.md) | Core apps in flash |
| [messaging](messaging.md) | Objects and mailboxes |
| [ports](ports.md) | Connections built on messaging |
| [connections](connections.md) | Connecting apps to services |
| [layout](layout.md) | What goes where on the card: `/apps`, `/data`, `/home` ... |
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
| [fonts](fonts.md) | Public-domain fonts for pages: Hershey stroke fonts, misc-fixed |
| [tts](tts.md) | Speech |
| [tts\_data](tts_data.md) | Speech data: `tools/speech` |
| [captions](captions.md) | System-wide captions |

## Graphics and the desktop

| Document | Subject |
|---|---|
| [window\_manager](window_manager.md) | `wm` developer guide |
| [dock](dock.md) | the dock's files: `wm.cfg` (what it shows) and `dock.zic` (its icons) |
| [responsive](responsive.md) | Staying responsive: long work in slices, progress, instant stop; `zco` coroutines, `zimgjob` |
| [widgets](widgets.md) | Widgets and dialogs |
| [libz](libz.md) | Runtime for zcc-compiled programs |
| [png](png.md) | PNG decoding |
| [svg](svg.md) | SVG rendering |
| [zcanvas](zcanvas.md) | Drawing pages: fills, strokes, patterns, images, bands; page shapes |
| [zpaper](zpaper.md) | Pages for printing: described by an app, played back by the print service |
| [remote\_desktop](remote_desktop.md) | Remote desktop in a browser: the ESP32 link, or `zerdesk` over Ethernet |

## Networking

| Document | Subject |
|---|---|
| [networking](networking.md) | The `net` service: IP, TCP, drivers |
| [netserve](netserve.md) | SSH, telnet and HTTP servers |
| [bbs](bbs.md) | The BBS and `fed` federation: plan, progress, running it |
| [fed](fed.md) | zfed, federation between BBS nodes: the protocol spec |
| [keccak\_hw](keccak_hw.md) | The Keccak-f[1600] block (optional) |
| [mlkem](mlkem.md) | ML-KEM-768 (FIPS 203): post-quantum key encapsulation |
| [zjson](zjson.md) | A strict JSON parser: zfed's subset |
| [z25519](z25519.md) | X25519 and Ed25519 checks on the montmul block |
| [sha256\_hw](sha256_hw.md) | The SHA-256 block, and how software shares it |
| [cryptobench](cryptobench.md) | What the cryptography costs on this machine |
| [bbs\_linux](bbs_linux.md) | The BBS as a Linux server: telnet, SSH via OpenSSH, systemd |
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
| [scheme\_api](scheme_api.md) | The Scheme API: adding procedures |
| [zapi](zapi.md) | Every Scheme procedure in `repl` (generated) |
| [basic\_app](basic_app.md) | `basic`: the BASIC computer |
| [editor](editor.md) | `te` in `repl` |
| [zcc](zcc.md) | The C compiler |
| [zcc\_bringup](zcc_bringup.md) | Testing zcc on hardware |
| [zfpga](zfpga.md) | FPGA toolchain on the machine itself |
| [zfpga-formats](zfpga-formats.md) | zfpga file formats |
| [zfpga-test](zfpga-test.md) | zfpga's first board test |

## Apps

| Document | Subject |
|---|---|
| [file\_browser](file_browser.md) | `files`: browse, find, copy, move, delete |
| [zwalk](zwalk.md) | find, copy and delete a folder tree a step at a time, stoppable at once |
| [text\_editor](text_editor.md) | `text` |
| [sheet\_app](sheet_app.md) | `sheet` |
| [slide\_app](slide_app.md) | `slide`: cards, slides and pages |
| [slide\_scripting](slide_scripting.md) | `slide`'s controls run Scheme: scripts, timers, the message box, example decks |
| [print](print.md) | `print`: printing over IPP or to port 9100, PDF and PostScript files |
| [convert](convert.md) | `convert`: documents, tables, archives |
| [app\_help](app_help.md) | F1: a help box in every app, with a button to these docs |
| [keys](keys.md) | Function keys: the same meaning in every app (F1 help ... F11 full screen, F12 app home) |
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
| [space3d\_app](space3d_app.md) | `space3d`: wireframe space game; 16-colour dual playfield in full screen |
| [snake\_app](snake_app.md) | `snake`: the classic, one or two players, window or full screen, 16 colours |
| [blocks\_app](blocks_app.md) | `blocks`: falling blocks; endless, 40 lines, 3 minutes; the stack drops by VRAM copies |
| [bricks\_app](bricks_app.md) | `bricks`: paddle, ball and a wall of bricks; twelve rounds, capsules, mouse or keys |
| [arcade](arcade.md) | The arcade runtime: one 320x240 game screen in a window or full screen, colour or mono |
| [sfx](sfx.md) | Game sound: effects and music on the hardware mixer |
| [chess\_app](chess_app.md) | `chess` |
| [chess\_engine](chess_engine.md) | The chess engine |
| [chip8\_app](chip8_app.md) | `chip8` |
| [mausrun](mausrun.md) | `mausrun` |
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
