# sw/apps/convert -- build rules are shared: sw/common/app.mk, docs/build.md.
#
# The formats themselves are sw/common/formats (docs/convert.md); the
# sheet's own file format and formula evaluator come from sw/apps/sheet,
# so a .zss written here is exactly what the sheet app reads and an
# xlsx made from one carries the values the sheet would show.
#
#   make                 convert.bin
#   make test            the formats library's host tests (Python,
#                        python-docx, openpyxl, odfpy, ebooklib; and
#                        LibreOffice when installed)

APP = convert

# Integer printf: nothing here prints a float, and it is 50KB.
ZFMT = 1

SRC_DIRS = ../../common/formats ../sheet
APP_CFLAGS = -I../../common

FORMATS = zf_core.o zf_xml.o zf_zip.o zf_em.o zf_docw.o zf_docx.o zf_rtf.o \
	zf_html.o zf_epub.o zf_text.o zf_tab.o zf_arch.o zf_conv.o

OBJS = zeitlos.o zobj.o zport.o zfsapp.o zwin.o zgfx.o zfont_data.o ztype.o \
	zinflate.o zfix.o sheet_core.o $(FORMATS) convert.o

# 64KB of stack and heap: the formats keep their state in .bss, and
# the largest frame is a few hundred bytes. The arena (zf_heap) is
# asked for separately and only while converting.
APP_STACK = 64K

test:
	python3 ../../common/formats/tests/run_tests.py

.PHONY: test

include ../../common/app.mk
