#!/usr/bin/env python3
# learn on Linux: the fonts learn loads (ldraw.c, ld_fonts_load()), built
# into the program so learn-linux runs from any folder.
#
#   mkfonts.py FONT-FOLDER OUT.c
#
# FONT-FOLDER is sw/data/font: zfn/ and zhf/ in it, and jp12.zfn.

import os
import sys

FILES = ["zfn/m9x15.zfn", "zfn/m9x15b.zfn", "zfn/m7x13.zfn", "zfn/m7x13b.zfn",
         "zfn/m10x20.zfn", "zfn/m6x13.zfn", "jp12.zfn",
         "zhf/serif.zhf", "zhf/serifb.zhf", "zhf/serifi.zhf", "zhf/sansb.zhf"]


def main():
    src, out = sys.argv[1], sys.argv[2]
    lines = ["// made by linux/mkfonts.py from sw/data/font: do not edit",
             "#include <stdint.h>", "#include <string.h>", ""]
    table = []
    for i, f in enumerate(FILES):
        data = open(os.path.join(src, f), "rb").read()
        lines.append("static const uint8_t f%d[%d] = {" % (i, len(data)))
        for k in range(0, len(data), 24):
            lines.append("\t" + ",".join(str(b) for b in data[k:k + 24]) + ",")
        lines.append("};")
        table.append('\t{ "%s", f%d, %d },' % (os.path.basename(f), i, len(data)))
    lines += ["", "static const struct { const char *name; const uint8_t *data; uint32_t len; } fonts[] = {"]
    lines += table
    lines += ["};", "",
              "// a font's file by its name, as ld_fonts_load() asks; NULL if it is not built in",
              "const void *ll_font(const char *file, uint32_t *len) {",
              "\tfor (unsigned i = 0; i < sizeof fonts / sizeof fonts[0]; i++)",
              "\t\tif (!strcmp(fonts[i].name, file)) { *len = fonts[i].len; return fonts[i].data; }",
              "\treturn NULL;",
              "}", ""]
    with open(out + ".tmp", "w") as o:
        o.write("\n".join(lines))
    os.replace(out + ".tmp", out)


if __name__ == "__main__":
    main()
