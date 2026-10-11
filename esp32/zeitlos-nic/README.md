# zeitlos-nic -- network firmware for the ESP32 link

ESP-IDF (v5.4, plain C) firmware for two boards from one source tree:

| board | chip | ZNIC (UART1, 3 Mbaud) | console | docs |
|---|---|---|---|---|
| ULX3S | ESP32, on the board | GPIO16/17 | UART0, via a passthru bitstream | `docs/esp32link.md` |
| Kabellos PMOD | ESP32-C3-WROOM-02 | GPIO6/7 (PMOD 9/10) | USB Serial/JTAG (micro-USB) | `docs/kabellos.md` |

It turns the module into the board's network interface for Zeitlos:
UART1 carries ZNIC frames to the FPGA, 802.11 and the IP gateway live
here. The board follows from the IDF target (`main/Kconfig.projbuild`);
see `docs/esp32link.md` in the repository root.

What it does:

- WiFi station: credentials arrive from Zeitlos (`NET.CFG` on the SD
  card, `STA` message); a `LINK` message reports the association.
- Gateway: a second lwIP netif at 192.168.4.1 faces Zeitlos
  (192.168.4.2); NAPT masquerades it behind the station address.
- TFTP server on 192.168.4.1: `test.bin`/`big.bin` (synthetic, for link
  tests), any `http://` or `https://` name is fetched and streamed, and
  uploads are counted -- `tget 192.168.4.1 https://host/file.txt f.txt`
  from Zeitlos downloads a URL.
- Serves the remote desktop: an HTTP server on port 80 with the viewer
  page, and a WebSocket relay for the framebuffer stripes `net` sends
  it and the keyboard and mouse the browser sends back. See
  `docs/remote_desktop.md`.
- ULX3S only: the SD card pins shared with the FPGA (GPIO 2/4/12-15)
  are left as inputs. (Never on the C3, where GPIO12-17 are its flash.)
- `ZTEST` CLI on the console -- UART0 (115200) on the ULX3S, USB on
  Kabellos -- for bring-up without Zeitlos (`esp32/nic_selftest.py`).
  `info` reports board, chip, the running image's ELF hash and free
  heap.

## Building

Needs ESP-IDF v5.4: `docs/esp32link.md`, "Installing ESP-IDF", has the
whole procedure (in short: clone `v5.4` to `~/esp/esp-idf`, run
`./install.sh esp32,esp32c3`).

```
python3 esp32/nic_build.py              # both boards
python3 esp32/nic_build.py ulx3s        # or one
python3 esp32/nic_build.py kabellos
```

or, for the ULX3S, as it always was:

```
cd esp32/zeitlos-nic
idf.py set-target esp32
idf.py build
```

The ULX3S builds into `build/` with the project's `sdkconfig`; Kabellos
into `build-kabellos/` with `build-kabellos/sdkconfig`, so the two never
overwrite each other (`nic_build.py`'s header says why). Each build also
writes a merged image for offset 0: `build/zeitlos-nic-ulx3s.bin`,
`build-kabellos/zeitlos-nic-kabellos.bin`. Kabellos-only settings live in
`sdkconfig.defaults.esp32c3`.

The viewer page (`web/index.html`) is embedded in the image, so editing
it means rebuilding and reflashing the firmware.

## Flashing Kabellos

Over the module's own USB, with it still in the PMOD port and the
Zeitlos desktop up (the FPGA holds the C3 in reset until `net` starts):

```
python3 esp32/kabellos_flash.py --build
python3 esp32/kabellos_flash.py --image zeitlos-nic-kabellos.bin   # a release image
```

`docs/kabellos.md`, "Flashing", is the reference. Everything below is
the ULX3S.

## Flashing the ULX3S

**This is not an ordinary ESP32 flash**, and the order matters: get one
step wrong and you are left with a board that looks dead. Four things
are true at once.

1. **The ESP32's UART0 does not reach USB.** It reaches the FPGA (pins
   K3/K4), so the FPGA has to be configured to pass it through. A
   "passthru" bitstream from
   <https://github.com/emard/ulx3s-passthru> does that and also drives
   GPIO0 low and EN high, which is download mode. Put it in
   `esp32/passthru/ulx3s_85f_passthru.bit`.
2. **The SD card has to come out.** Its DAT0 line is the ESP32's GPIO2,
   which is a boot strap: with a card in the slot, the chip will not
   enter download mode.
3. **The card cannot be pulled live**, so the board gets unplugged --
   twice, once to take the card out and once to put it back.
4. **The FPGA's configuration lives in SRAM** and every unplug erases
   it. So the last step is always loading `soc.bit` again; and until
   you do, the ESP32 is held in reset by a blank FPGA, which is why a
   board that "disappeared from the network" right after a flash is
   almost always this and not a network problem.

`esp32/flash.py` does the parts a machine can do and stops at the two
that need hands:

```
python3 esp32/flash.py --host <the board's IP>
```

```
[1/8] checks                     (nothing touched yet)
[2/8] unplug -> card out -> plug in
[3/8] passthru bitstream         -> download mode
[4/8] esptool write_flash        -> offsets from build/flash_args
[5/8] unplug -> card in -> plug in
[6/8] reload soc.bit
[7/8] wait for the desktop
[8/8] is it serving the new page?
```

At steps 2 and 5 it does not take your word for it: it watches the
serial device node disappear and come back, which is the honest signal
that the board really lost power, because the board is powered over
the cable that carries it.

What it checks before touching anything: that the files the build
recorded in `build/flash_args` are all there (read, not hardcoded -- a
partition table that grows moves the app, and a copy of the offsets
here would keep flashing the old layout without saying so); that
`build/zeitlos-nic.bin` is newer than `web/index.html`, so you cannot
flash a stale copy of a page you just edited (`--build` rebuilds,
`--force-stale` goes ahead anyway); that the page is really inside the
image; and that nothing is holding the serial port, since the FT231X
cannot do JTAG and UART at once. Afterwards it checks the far end: that
the desktop answers, that the page it serves carries the marker the
image was checked for, and that the framebuffer is still streaming.

Useful flags: `--build` rebuilds first; `--from <stage>` picks a run up
where it stopped (`flash` with the card already out, `soc` with it back
in, `verify` to only check a board that is already running); `--log`
keeps a transcript; `--port` and `--ftdi-serial` matter only with more
than one board attached. `openFPGALoader` must be on `PATH`.

Without `--host` (or `ZEITLOS_HOST`) the flash still happens, but
nothing is verified: the address is the one the access point handed the
ESP32 and nothing here can work it out. The console prints it at
1 Mbaud -- `esp_netif_handlers: sta ip: ...` -- and `net` writes it to
`NET.IP` at the root of the sdcard.

### When it goes wrong

| symptom | almost always |
| --- | --- |
| `esptool` reports `Wrong boot mode detected (0x1a)` | the SD card is in, holding GPIO2. Take it out, or power-cycle without it |
| `esptool` finds no chip at all | no passthru bitstream loaded, or the wrong one for this FPGA size |
| the port is busy | a console or monitor has it; the script names the process |
| nothing answers afterwards | `soc.bit` was not reloaded, or the board joined a different network |
| it answers, with the old page | the write did not take; run it again |
| `esptool` dies part-way through | power. Use a USB port that can supply the current, not a bus-powered hub |

`Wrong boot mode` in particular also turns up with the card *out* if
the FPGA was reprogrammed in the middle of an SD transfer, because the
card can be left driving DAT0. A power cycle without the card clears
it.

### Flashing by hand

The script is not required; it is the order that is. Given a build and
a passthru bitstream, with the card out:

```
openFPGALoader -b ulx3s esp32/passthru/ulx3s_85f_passthru.bit
cd esp32/zeitlos-nic/build
python3 -m esptool --chip esp32 -p <port> -b 460800 write_flash $(cat flash_args)
```

(from inside `build`, because the names in `flash_args` are relative to
it)

then card back in, and `openFPGALoader -b ulx3s output/ulx3s/soc.bit`.
`esptool` comes from the ESP-IDF python environment.

## Self-test without Zeitlos

`esp32/nic_selftest.py` loads the passthru bitstream, optionally
flashes, and drives the `ZTEST` CLI on UART0:

```
python3 esp32/nic_selftest.py --flash --ssid MyAP --psk secret
```

`test <ssid> <psk>` runs scan, association, DHCP and a ping to
8.8.8.8; `inject gw|ping|dns`, `poll` and `sta2` exercise the gateway.
Reload the Zeitlos bitstream afterwards.

`--board kabellos` runs the same test over the C3's USB console, with no
passthru and no reset.

Notes: `sdkconfig.defaults` sets the event task stack (4096), the
tcpip task stack (8192), core locking and NAPT; keep them. The
python.org 3.10 interpreter aborts at exit on recent macOS; run the
IDF venv with `PYTHONMALLOC=malloc` (the scripts do).
