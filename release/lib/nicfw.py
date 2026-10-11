"""Network-module firmware that ships with a release.

A Kabellos target's gateware is half of a working network link; the
other half is zeitlos-nic running on the PMOD's ESP32-C3. The module
ships with Espressif's ESP-AT firmware, which Zeitlos cannot talk to,
so a release that offers lakritz_kabellos without the firmware offers
an image whose network never comes up. The firmware goes in dist/ as
`zeitlos-nic-kabellos.bin`, a merged image for offset 0x0, which
esp32/kabellos_flash.py --image writes with nothing but esptool.

Which firmware a target needs follows from its defines: `ESP32_PMOD
means "an ESP32 on a PMOD", which today means Kabellos. The ULX3S's
on-board ESP32 (`ESP32_LINK without `ESP32_PMOD) is not built here: its
flashing procedure needs the board in hand (esp32/flash.py), and the
release notes have always said its image covers the FPGA only.

Built CLEAN, always. esp32/zeitlos-nic/build-kabellos/sdkconfig is where
`idf.py menuconfig` leaves a developer's local changes -- a raised TX
power, say -- and a release has to be the tree's defaults, not
somebody's experiment. docs/releases.md, "Network module firmware".
"""

import os
import shutil
import sys

# Which firmware board a define selects. One entry; a second ESP32 PMOD
# with different pins would be a second entry and a second Kconfig board.
_BY_DEFINE = {"ESP32_PMOD": "kabellos"}


def _nic_build(root):
    path = os.path.join(root, "esp32")
    if path not in sys.path:
        sys.path.insert(0, path)
    import nic_build
    return nic_build


def asset_name(board):
    return "zeitlos-nic-%s.bin" % board


def needed(targets):
    """{firmware board: [target names]} for these loaded targets."""
    out = {}
    for t in targets:
        for define, board in _BY_DEFINE.items():
            if define in t.defines:
                out.setdefault(board, []).append(t.name)
    return out


def preflight(root, idf):
    """The ESP-IDF to build with, or a reason there is none.

    Called before any gateware is built: finding out after an hour of
    place-and-route that the last step cannot run is the wrong order.
    Returns (idf_dir or None-if-on-PATH, error message or None)."""
    nb = _nic_build(root)
    try:
        idf_dir = nb.find_idf(idf)
        if idf_dir:
            nb.idf_env(idf_dir)	# the python environment exists too
        return idf_dir, None
    except SystemExit as e:
        return None, str(e)


def build_into(root, boards, idf_dir, out):
    """Build each firmware board clean and copy its merged image to out.

    Returns {board: asset filename}."""
    nb = _nic_build(root)
    made = {}
    for board in sorted(boards):
        print("\n=== zeitlos-nic for %s (clean build) ===" % board)
        img = nb.build(board, idf_dir, True)
        name = asset_name(board)
        shutil.copy2(img, os.path.join(out, name))
        made[board] = name
    return made
