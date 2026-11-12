#!/usr/bin/env python3
# Catalogs and a 5x8 glyph file for test_zi18n.c, made by tools/i18n.py's
# own writer, so the C reader is tested against what the tool writes.
import os, struct, sys
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "..", "tools"))
import i18n

out = sys.argv[1]
de = {
    "text": {
        (None, "Save as...", None): ["Speichern unter..."],
        ("verb", "Open", None): ["Öffnen"],
        (None, "%d file", "%d files"): ["%d Datei", "%d Dateien"],
        (None, "Files", None): ["Dateien"],
    },
    "common": {
        (None, "Cancel", None): ["Abbrechen"],
        (None, "Save as...", None): ["(common's)"],
    },
}
ru = {"text": {(None, "%d file", "%d files"): ["%d файл", "%d файла", "%d файлов"],
               (None, "Files", None): ["Файлы"]}}
ja = {"text": {(None, "Files", None): ["ファイル"], (None, "Save as...", None): ["名前を付けて保存"]}}
open(out + "/de.zlc", "wb").write(i18n.build_catalog("de", 1, de))
open(out + "/ru.zlc", "wb").write(i18n.build_catalog("ru", 3, ru))
open(out + "/ja.zlc", "wb").write(i18n.build_catalog("ja", 0, ja))
# a 5x8 ZFN1 with the Cyrillic of "Файлы" (and nothing Japanese)
cps = sorted(set(ord(c) for c in "Файлы"))
f = bytearray(b"ZFN1") + struct.pack("<BBBBI", 5, 8, 1, 0, len(cps))
for c in cps: f += struct.pack("<H", c)
while len(f) % 4: f += b"\0"
for c in cps: f += bytes([0xF8] * 8)
open(out + "/s58.zfn", "wb").write(f)
