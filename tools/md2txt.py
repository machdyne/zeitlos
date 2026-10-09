#!/usr/bin/env python3
#
# Zeitlos
#
# Renders a Markdown document from docs/ as plain text for the flash
# underlay (docs/flash_apps.md, "Files in flash"), to be read in `text`
# on a machine with no card -- so with no `read` to render Markdown.
#
#   tools/md2txt.py docs/welcome.md output/files/docs/welcome.txt
#
# `text` wraps a long line itself, and treats each line as a paragraph,
# so a paragraph or a list item becomes ONE line here, however many it
# took in the source. Headings are underlined, tables become aligned
# columns, code blocks are kept as they are, indented, and the markup
# that only means something to a renderer -- emphasis, backticks, link
# targets, HTML comments -- is dropped, keeping the words.

import re
import sys


def inline(s):
    s = re.sub(r"!\[([^\]]*)\]\([^)]*\)", r"\1", s)          # images: alt text
    s = re.sub(r"\[([^\]]+)\]\([^)]*\)", r"\1", s)           # links: their text
    s = re.sub(r"\*\*([^*]+)\*\*", r"\1", s)                  # bold
    s = re.sub(r"(?<![\w*])\*([^*\s][^*]*)\*(?![\w*])", r"\1", s)   # italic
    s = re.sub(r"`([^`]+)`", r"\1", s)                        # code
    s = s.replace("\\|", "|")
    return s.strip()


def table(rows):
    cells = [[inline(c) for c in r.strip().strip("|").split("|")] for r in rows]
    cells = [r for r in cells if not all(re.fullmatch(r":?-+:?", c.strip()) for c in r)]
    headed = bool(cells) and any(c for c in cells[0])
    if cells and not headed:
        cells = cells[1:]                  # an empty header row: none
    if not cells:
        return []
    n = max(len(r) for r in cells)
    for r in cells:
        r += [""] * (n - len(r))
    # every column but the last padded to its width -- unless that is
    # too wide to leave the last one room, when a row is "a: b"
    widths = [max(len(r[i]) for r in cells) for i in range(n - 1)]
    out = []
    if sum(widths) + 2 * len(widths) > 40:
        for r in (cells[1:] if headed and len(cells) > 1 else cells):
            out.append(r[0] + ": " + "  ".join(c for c in r[1:] if c))
        return out
    for k, r in enumerate(cells):
        line = "".join(r[i].ljust(widths[i] + 2) for i in range(n - 1)) + r[-1]
        out.append(line.rstrip())
        if k == 0 and headed and len(cells) > 1:
            out.append("".join("-" * widths[i] + "  " for i in range(n - 1)) +
                       "-" * min(20, max(len(r[-1]) for r in cells)))
    return out


def render(md):
    md = re.sub(r"<!--.*?-->", "", md, flags=re.S)
    lines = md.splitlines()
    out = []
    para = []
    i = 0

    def flush():
        if para:
            out.append(inline(" ".join(para)))
            para.clear()

    def blank():
        if out and out[-1] != "":
            out.append("")

    while i < len(lines):
        line = lines[i]
        s = line.strip()

        if s.startswith("```"):
            flush(); blank()
            i += 1
            while i < len(lines) and not lines[i].strip().startswith("```"):
                out.append("    " + lines[i].rstrip())
                i += 1
            i += 1
            blank()
            continue

        m = re.match(r"(#{1,6})\s+(.*)", s)
        if m:
            flush(); blank()
            text = inline(m.group(2))
            out.append(text)
            if len(m.group(1)) <= 2:
                out.append(("=" if len(m.group(1)) == 1 else "-") * len(text))
            blank()
            i += 1
            continue

        if s.startswith("|"):
            flush(); blank()
            rows = []
            while i < len(lines) and lines[i].strip().startswith("|"):
                rows.append(lines[i])
                i += 1
            out.extend(table(rows))
            blank()
            continue

        m = re.match(r"(\s*)([-*+]|\d+[.)])\s+(.*)", line)
        if m:
            flush()
            item = [m.group(3)]
            indent = len(m.group(1))
            i += 1
            # continuation lines: indented further, not a new item
            while i < len(lines) and lines[i].strip() and \
                    not re.match(r"\s*([-*+]|\d+[.)])\s+", lines[i]) and \
                    (len(lines[i]) - len(lines[i].lstrip())) > indent:
                item.append(lines[i].strip())
                i += 1
            bullet = m.group(2) if m.group(2)[0].isdigit() else "-"
            out.append("  " * (indent // 2) + bullet + " " + inline(" ".join(item)))
            continue

        if not s:
            flush(); blank()
            i += 1
            continue

        if s.startswith(">"):
            s = s.lstrip(">").strip()
        if re.fullmatch(r"(-{3,}|\*{3,}|_{3,})", s):
            flush(); blank()
            i += 1
            continue
        para.append(s)
        i += 1

    flush()
    while out and out[-1] == "":
        out.pop()
    return "\n".join(out) + "\n"


def main(argv):
    if len(argv) != 3:
        print("usage: md2txt.py IN.md OUT.txt", file=sys.stderr)
        return 1
    with open(argv[1], encoding="utf-8") as f:
        text = render(f.read())
    import os
    d = os.path.dirname(argv[2])
    if d:
        os.makedirs(d, exist_ok=True)
    with open(argv[2], "w", encoding="utf-8") as f:
        f.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
