# zeitlos-nic -- ESP32 firmware for the ULX3S network link

ESP-IDF (v5.4, plain C) firmware for the ESP32 on the ULX3S. It turns
the module into the board's network interface for Zeitlos: UART1
(GPIO16/17, 1 Mbaud) carries ZNIC frames to the FPGA, 802.11 and the
IP gateway live here. See `docs/esp32link.md` in the repository root.

What it does:

- WiFi station: credentials arrive from Zeitlos (`NET.CFG` on the SD
  card, `STA` message); a `LINK` message reports the association.
- Gateway: a second lwIP netif at 192.168.4.1 faces Zeitlos
  (192.168.4.2); NAPT masquerades it behind the station address.
- TFTP server on 192.168.4.1: `test.bin`/`big.bin` (synthetic, for link
  tests), any `http://` or `https://` name is fetched and streamed, and
  uploads are counted -- `tget 192.168.4.1 https://host/file.txt f.txt`
  from Zeitlos downloads a URL.
- The SD card pins shared with the FPGA (GPIO 2/4/12-15) are left as
  inputs.
- `ZTEST` CLI on UART0 (115200) for bring-up without Zeitlos
  (`esp32/nic_selftest.py`).

Build and flash (the FPGA must expose the ESP32's UART0 to the USB
serial; a "passthru" bitstream from https://github.com/emard/ulx3s-passthru
does that -- put it in `esp32/passthru/ulx3s_85f_passthru.bit`):

```
cd esp32/zeitlos-nic
idf.py set-target esp32
idf.py build
python3 ../nic_selftest.py --flash --ssid MyAP --psk secret
```

`nic_selftest.py` loads the passthru bitstream, flashes the firmware,
and runs `test <ssid> <psk>` (scan, associate, DHCP, ping 8.8.8.8).
Reload the Zeitlos bitstream afterwards.

Notes: `sdkconfig.defaults` sets the event task stack (4096), the
tcpip task stack (8192), core locking and NAPT; keep them. The
python.org 3.10 interpreter aborts at exit on recent macOS; run the
IDF venv with `PYTHONMALLOC=malloc` (the script does).
