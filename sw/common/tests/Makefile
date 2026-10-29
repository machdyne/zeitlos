# Host tests for sw/common that no app's Makefile builds. `make test`
# here, or `make test` at the top of the tree for every suite
# (tools/run_tests.py, docs/build.md).
#
# Each recipe is the one in its test's header comment, run from the top
# of the tree. Binaries and generated corpora go to $(OUT). Exit 77 from
# a test is a skip (the trampoline tests need vm.mmap_min_addr=0).
#
# The Makefile.* files beside this one are separate suites, run as
# `make -f Makefile.<name>`; tools/run_tests.py runs those too.

R = ../../..
OUT ?= /tmp/zeitlos-common-tests
HOSTCC ?= cc
CC_ = cd $(R) && $(HOSTCC) -std=gnu99
SKIP77 = r=$$?; [ $$r -eq 0 ] || { [ $$r -eq 77 ] && echo "  (skipped)"; }

TESTS = accent args bitbang dialog flist gfx_region gz inflate png scrollbar text undo vt \
	zcaption zcfg zexec zexec_inflate zexec_lz4 zfmt zinput zkbd zscreen \
	zsha1 ztype zwalk zws

.PHONY: test $(TESTS) clean

test: $(TESTS)

$(OUT):
	mkdir -p $(OUT)

accent: | $(OUT)
	$(CC_) -Wall -I sw/common -o $(OUT)/test_accent sw/common/tests/test_accent.c \
		sw/common/zaccent.c && $(OUT)/test_accent

args: | $(OUT)
	$(CC_) -Wall -I sw/common -o $(OUT)/test_args sw/common/tests/test_args.c && $(OUT)/test_args

bitbang: | $(OUT)
	$(CC_) -I sw/common -o $(OUT)/test_bitbang sw/common/tests/test_bitbang.c \
		sw/common/zi2c.c sw/common/zspi.c sw/common/zgpio.c && $(OUT)/test_bitbang

dialog: | $(OUT)
	$(CC_) -Wall -no-pie -I sw/common -o $(OUT)/test_dialog \
		sw/common/tests/test_dialog.c sw/common/zwin.c sw/common/zwidget.c \
		sw/common/zflist.c sw/common/zedit.c sw/common/zfsapp.c \
		sw/common/zfont_data.c sw/common/zobj.c sw/common/zeitlos.c \
		sw/common/tests/zspeak_stub.c
	@$(OUT)/test_dialog; $(SKIP77)

flist: | $(OUT)
	$(CC_) -Wall -no-pie -I sw/common -o $(OUT)/test_flist \
		sw/common/tests/test_flist.c sw/common/zflist.c sw/common/zwin.c \
		sw/common/zwidget.c sw/common/zfsapp.c sw/common/zfont_data.c \
		sw/common/zobj.c sw/common/zeitlos.c sw/common/zspeak.c
	@$(OUT)/test_flist $(OUT)/flist; $(SKIP77)

gfx_region: | $(OUT)
	$(CC_) -o $(OUT)/test_gfx_region sw/common/tests/test_gfx_region.c && $(OUT)/test_gfx_region

gz: | $(OUT)
	cd $(R) && python3 sw/common/tests/gen_inflate_corpus.py $(OUT)/gzc > /dev/null
	$(CC_) -I sw/common -o $(OUT)/test_gz sw/common/tests/test_gz.c \
		sw/common/zgz.c sw/common/zinflate.c && $(OUT)/test_gz $(OUT)/gzc

inflate: | $(OUT)
	cd $(R) && python3 sw/common/tests/gen_inflate_corpus.py $(OUT)/gzc > /dev/null
	$(CC_) -I sw/common -o $(OUT)/test_inflate sw/common/tests/test_inflate.c \
		sw/common/zinflate.c && $(OUT)/test_inflate $(OUT)/gzc

# The corpus needs Pillow; skipped without it.
png: | $(OUT)
	@if ! python3 -c "import PIL" 2>/dev/null; then echo "test_png: skipped (no Pillow)"; exit 0; fi; \
	cd $(R) && python3 sw/common/tests/gen_png_corpus.py $(OUT)/pngc > /dev/null && \
	$(HOSTCC) -std=gnu99 -DZ_IMG_HAVE_PNG=1 -I sw/common -o $(OUT)/test_png \
		sw/common/tests/test_png.c sw/common/zimg.c sw/common/zinflate.c && \
	$(OUT)/test_png $(OUT)/pngc

scrollbar: | $(OUT)
	$(CC_) -Wall -no-pie -I sw/common -I sw/common/tests -o $(OUT)/test_scrollbar \
		sw/common/tests/test_scrollbar.c sw/common/zwin.c sw/common/zwidget.c \
		sw/common/zfont_data.c sw/common/zobj.c sw/common/zeitlos.c \
		sw/common/tests/zspeak_stub.c
	@$(OUT)/test_scrollbar; $(SKIP77)

text: | $(OUT)
	$(CC_) -Wall -no-pie -I sw/common -o $(OUT)/test_text \
		sw/common/tests/test_text.c sw/common/zfont_data.c && $(OUT)/test_text

undo: | $(OUT)
	$(CC_) -Wall -I sw/common -o $(OUT)/test_undo sw/common/tests/test_undo.c && $(OUT)/test_undo

vt: | $(OUT)
	$(CC_) -Wall -I sw/common -o $(OUT)/test_vt sw/common/tests/test_vt.c \
		sw/common/zvt100.c && $(OUT)/test_vt

zcaption: | $(OUT)
	$(CC_) -Wall -DZCAPTION_HOST -I sw/common -o $(OUT)/test_zcaption \
		sw/common/tests/test_zcaption.c sw/common/zcaption.c sw/common/zfont_data.c && \
	$(OUT)/test_zcaption $(OUT)/cap

zcfg: | $(OUT)
	$(CC_) -Wall -DZCFG_KERNEL -I sw/common -o $(OUT)/test_zcfg \
		sw/common/tests/test_zcfg.c sw/common/zcfg.c sw/common/zrtc.c && $(OUT)/test_zcfg

zexec: | $(OUT)
	$(CC_) -Wall -I sw/common -o $(OUT)/test_zexec sw/common/tests/test_zexec.c && $(OUT)/test_zexec

zexec_inflate: | $(OUT)
	$(CC_) -Wall -g -fsanitize=address,undefined -I sw/common -o $(OUT)/test_zexec_inflate \
		sw/common/tests/test_zexec_inflate.c sw/common/zexec_inflate.c -lz && \
	$(OUT)/test_zexec_inflate

zexec_lz4: | $(OUT)
	$(CC_) -Wall -g -fsanitize=address,undefined -I sw/common -o $(OUT)/test_zexec_lz4 \
		sw/common/tests/test_zexec_lz4.c sw/common/zexec_lz4.c && $(OUT)/test_zexec_lz4

# with and without floating point
zfmt: | $(OUT)
	$(CC_) -Wall -DZFMT_HOST_TEST -I sw/common -o $(OUT)/test_zfmt \
		sw/common/tests/test_zfmt.c sw/common/zfmt.c && $(OUT)/test_zfmt
	$(CC_) -Wall -DZFMT_HOST_TEST -DZFMT_FLOAT -I sw/common -o $(OUT)/test_zfmt_f \
		sw/common/tests/test_zfmt.c sw/common/zfmt.c -lm && $(OUT)/test_zfmt_f

zinput: | $(OUT)
	$(CC_) -Wall -Wextra -I sw/common -o $(OUT)/test_zinput \
		sw/common/tests/test_zinput.c sw/common/zinput.c && $(OUT)/test_zinput

zkbd: | $(OUT)
	$(CC_) -Wall -I sw/common -o $(OUT)/test_zkbd sw/common/tests/test_zkbd.c \
		sw/common/zkbd.c && $(OUT)/test_zkbd

zscreen: | $(OUT)
	$(CC_) -Wall -Wextra -I sw/common -I sw/apps/net -o $(OUT)/test_zscreen \
		sw/common/tests/test_zscreen.c sw/common/zscreen.c && $(OUT)/test_zscreen

zsha1: | $(OUT)
	$(CC_) -Wall -I sw/common -o $(OUT)/test_zsha1 sw/common/tests/test_zsha1.c \
		sw/common/zsha1.c && $(OUT)/test_zsha1

ztype: | $(OUT)
	$(CC_) -Wall -I sw/common -o $(OUT)/test_ztype sw/common/tests/test_ztype.c \
		sw/common/ztype.c && $(OUT)/test_ztype

zwalk: | $(OUT)
	$(CC_) -Wall -I sw/common -o $(OUT)/test_zwalk sw/common/tests/test_zwalk.c \
		sw/common/zwalk.c && $(OUT)/test_zwalk

zws: | $(OUT)
	$(CC_) -Wall -Wextra -I sw/common -o $(OUT)/test_zws sw/common/tests/test_zws.c \
		sw/common/zws.c sw/common/zsha1.c && $(OUT)/test_zws

clean:
	rm -rf $(OUT)
