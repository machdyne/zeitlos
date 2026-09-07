# Zeitlos -- sw/apps/web
#
# An experimental text-mode web browser, and the system's fetch
# service. See docs/web_app.md.
#
#   make            build web.bin
#   make test       the host test suite -- url, http, html, layout, page
#   make render     draw a page off-device and write /tmp/web.pbm
#
# The host targets are not an afterthought here, they are the strategy.
# Everything with interesting logic in it -- URL resolution, the HTTP
# parser, the HTML tokenizer, wrapping, the checkpoint index -- is
# written free of Zeitlos dependencies precisely so it can be run on
# the build machine against real input. web.c itself is the part that
# genuinely needs the machine, and it is deliberately the smallest
# part.

PREFIX ?= $(RISCV_PREFIX)
# ARCH/ABI come from one place -- see that file for why
include ../../common/arch.mk
CC_TARGET = $(PREFIX)gcc
AS = $(PREFIX)as
ASFLAGS = -march=$(ARCH) -mabi=$(ABI)

# -- TLS --
#
# ON by default. There is no build of this app that speaks TLS
# without verifying certificates: the flag that allowed that while
# the verification machinery was being written has been removed,
# along with every code path it guarded.
#
#   make WEB_TLS=0
#
# builds without TLS at all -- `https://` then reports that this build
# has no TLS and stops. That is the option for a board where the extra
# crypto does not fit, not a way to switch checking off.
#
# See docs/tls.md and docs/x509.md.
WEB_TLS ?= 1

GC_SECTIONS ?= 1
ifeq ($(GC_SECTIONS),1)
GC_CFLAGS = -ffunction-sections -fdata-sections
GC_LDFLAGS = -Wl,--gc-sections
endif

# -DZ_GFX_HW_BLIT, same as read/wm/term: the widget toolkit reads its
# text labels through the hardware glyph blitter, and this app draws a
# great many glyphs -- a full screen of 5x8 text is around 2,600 of
# them. In software that is roughly 5,300 cycles each. See
# docs/window_manager.md, "Hardware glyph blitting".
EXTRA_CFLAGS ?=

# `override` for the reason every other app in this tree says so: a
# command-line CFLAGS REPLACES this one rather than adding to it,
# which silently drops -Os, -march, section GC and -DZ_GFX_HW_BLIT.
# The result still builds and still links; it just renders every glyph
# in software and triples in size, which can overrun the space the
# loader has for the app.
# The crypto is built at -O2, everything else at -Os.
#
# Not a guess about code size: on this CPU a Montgomery multiply costs
# roughly 337 cycles per 32x32 multiply-accumulate, measured, against
# a DSP multiplier that takes 2. Almost all of that is instruction
# fetch and load/store on an in-order core -- so the thing to remove
# is INSTRUCTIONS, and -O2 unrolls the fixed-length inner loops that
# -Os keeps rolled.
#
# Scoped to the four files that do the arithmetic. Applying it
# everywhere would grow an app that is already the largest in the tree
# for no benefit: nothing else here is in a loop that runs millions of
# times.
CRYPTO_CFLAGS = -O2

# The hardware Montgomery multiplier path in ecdsa.c (rtl/montmul.v).
#
# TARGET BUILDS ONLY. The host tests must not have it: they would
# dereference the MMIO base and segfault. Opt-in rather than opt-out
# for exactly that reason.
#
# Enabling it here does not require the block to exist -- ecdsa.c
# checks the feature bit, the block's MAGIC and CONFIG, and runs a
# self-test before using it, and falls back to software otherwise.
override CFLAGS += -DEC_HW

# Forces the software image blit (layout.c's blit_rows()).
#
#   make WEB_IMG_SW=1
#
# The way to tell a decode fault from a blit fault without a logic
# analyser: if an image looks right this way and wrong without it, the
# hardware blitter call is where to look, not zimg.
WEB_IMG_SW ?= 0
override CFLAGS += -DWEB_IMG_SW=$(WEB_IMG_SW)

override CFLAGS += --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=$(ABI) \
	-DZ_GFX_HW_BLIT -DWEB_TLS=$(WEB_TLS) \
	$(GC_CFLAGS) $(ARCH_DEFS) $(LIBC_FLAGS) $(EXTRA_CFLAGS)
LDFLAGS = -march=$(ARCH) -mabi=$(ABI) $(GC_LDFLAGS) $(LIBC_FLAGS)
LDSCRIPT = ../../common/riscv-app.ld

# url.o, uni.o, html.o, layout.o, page.o and http.o are all free of
# I/O and of Zeitlos types (layout.o and page.o take zgfx.h and
# zfont.h for the drawing primitives and nothing else), which is what
# lets the `test` target below build them for the host.
ifeq ($(WEB_TLS), 1)
TLS_OBJ = tls.o tls_crypto.o zsha256.o monocypher.o zrng.o \
	          der.o x509.o bignum.o rsa.o ecdsa.o sha384.o \
	          monocypher-ed25519.o verify.o
else
TLS_OBJ =
endif

OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zwin.o zwidget.o \
	zfsapp.o zflist.o zedit.o zinflate.o zimg.o zport.o zstream.o zdns.o \
	url.o uni.o http.o html.o layout.o page.o toolbar.o $(TLS_OBJ) web.o

web: web.elf web.bin

# WEB_TLS changes CFLAGS without changing a single source file, so
# make on its own would happily relink a mixture of objects built with
# and without TLS. This stamp holds the current value; web.o lists it
# as a prerequisite, so changing the option changes the stamp's mtime
# and web.o rebuilds.
#
# The first version of this rule did `rm -f` on the objects instead.
# That is subtly and badly wrong: web.o is the LAST entry in $(OBJS),
# so make built every other object first, then ran this recipe as
# web.o's prerequisite and deleted all of them -- and the link failed
# with "cannot find zeitlos.o". Writing the stamp and letting mtime do
# the work is what sw/apps/net/Makefile already does; there was no
# reason to invent something else.
.web_config_selected: FORCE
	@cur="$(WEB_TLS)"; \
	if [ ! -f $@ ] || [ "$$(cat $@ 2>/dev/null)" != "$$cur" ]; then \
		echo "$$cur" > $@; \
	fi

FORCE:
.PHONY: FORCE

zeitlos.o:
	$(CC_TARGET) $(CFLAGS) -c ../../common/zeitlos.c -o zeitlos.o

zobj.o:
	$(CC_TARGET) $(CFLAGS) -c ../../common/zobj.c -o zobj.o

zgfx.o:
	$(CC_TARGET) $(CFLAGS) -c ../../common/zgfx.c -o zgfx.o

zfont_data.o:
	$(CC_TARGET) $(CFLAGS) -c ../../common/zfont_data.c -o zfont_data.o

zwin.o:
	$(CC_TARGET) $(CFLAGS) -c ../../common/zwin.c -o zwin.o

zwidget.o:
	$(CC_TARGET) $(CFLAGS) -c ../../common/zwidget.c -o zwidget.o

zfsapp.o:
	$(CC_TARGET) $(CFLAGS) -c ../../common/zfsapp.c -o zfsapp.o

zflist.o:
	$(CC_TARGET) $(CFLAGS) -c ../../common/zflist.c -o zflist.o

# sw/common/zedit.c -- the URL field. web no longer links zdialog.c
# at all: the toolbar replaced the modal prompt, and the text field it
# used moved here when web became its second caller.
zedit.o:
	$(CC_TARGET) $(CFLAGS) -c ../../common/zedit.c -o zedit.o

# sw/common/zinflate.c -- gzip for Content-Encoding, and what a PNG
# decoder will need, which is why it lives in common.
zinflate.o:
	$(CC_TARGET) $(CFLAGS) -c ../../common/zinflate.c -o zinflate.o

# sw/common/zimg.c -- the image decoders, for in-place images. PNG is
# off until its decoder exists; BMP, PNM, GIF and JPEG are on, which
# is what the in-place path is being tested against.
zimg.o:
	$(CC_TARGET) $(CFLAGS) -c ../../common/zimg.c -o zimg.o

zport.o:
	$(CC_TARGET) $(CFLAGS) -c ../../common/zport.c -o zport.o

zstream.o:
	$(CC_TARGET) $(CFLAGS) -c ../../common/zstream.c -o zstream.o

zdns.o:
	$(CC_TARGET) $(CFLAGS) -c ../../common/zdns.c -o zdns.o

url.o: url.c url.h
	$(CC_TARGET) $(CFLAGS) -c url.c -o url.o

uni.o: uni.c uni.h
	$(CC_TARGET) $(CFLAGS) -c uni.c -o uni.o

http.o: http.c http.h url.h
	$(CC_TARGET) $(CFLAGS) -c http.c -o http.o

# The browser toolbar's geometry. Its own file so it can be tested
# without a window or a framebuffer -- see tests/test_toolbar.c.
toolbar.o: toolbar.c toolbar.h
	$(CC_TARGET) $(CFLAGS) -c toolbar.c -o toolbar.o

html.o: html.c html.h uni.h
	$(CC_TARGET) $(CFLAGS) -c html.c -o html.o

layout.o: layout.c layout.h html.h
	$(CC_TARGET) $(CFLAGS) -c layout.c -o layout.o

page.o: page.c page.h html.h layout.h
	$(CC_TARGET) $(CFLAGS) -c page.c -o page.o

tls_crypto.o: tls_crypto.c tls_crypto.h ../../common/zsha256.h
	$(CC_TARGET) $(CFLAGS) -c tls_crypto.c -o tls_crypto.o

tls.o: tls.c tls.h tls_crypto.h
	$(CC_TARGET) $(CFLAGS) -c tls.c -o tls.o

der.o: der.c der.h
	$(CC_TARGET) $(CFLAGS) -c der.c -o der.o

x509.o: x509.c x509.h der.h
	$(CC_TARGET) $(CFLAGS) -c x509.c -o x509.o

bignum.o: bignum.c bignum.h
	$(CC_TARGET) $(CFLAGS) $(CRYPTO_CFLAGS) -c bignum.c -o bignum.o

rsa.o: rsa.c rsa.h bignum.h ../../common/zsha256.h
	$(CC_TARGET) $(CFLAGS) $(CRYPTO_CFLAGS) -c rsa.c -o rsa.o

ecdsa.o: ecdsa.c ecdsa.h
	$(CC_TARGET) $(CFLAGS) $(CRYPTO_CFLAGS) -c ecdsa.c -o ecdsa.o

sha384.o: sha384.c sha384.h
	$(CC_TARGET) $(CFLAGS) -c sha384.c -o sha384.o

# SHA-512, which sha384.c builds on. --gc-sections drops the Ed25519
# half of this object, which nothing here calls.
monocypher-ed25519.o:
	$(CC_TARGET) $(CFLAGS) -c ../../ext/monocypher/monocypher-ed25519.c \
		-o monocypher-ed25519.o

verify.o: verify.c verify.h x509.h rsa.h ecdsa.h sha384.h
	$(CC_TARGET) $(CFLAGS) -c verify.c -o verify.o

zsha256.o:
	$(CC_TARGET) $(CFLAGS) $(CRYPTO_CFLAGS) -c ../../common/zsha256.c -o zsha256.o

monocypher.o:
	$(CC_TARGET) $(CFLAGS) -c ../../ext/monocypher/monocypher.c -o monocypher.o

zrng.o:
	$(CC_TARGET) $(CFLAGS) -c ../../common/zrng.c -o zrng.o

web.o: web.c url.h http.h html.h layout.h page.h ../../common/zweb.h \
	.web_config_selected
	$(CC_TARGET) $(CFLAGS) -c web.c -S -o web.asm
	$(CC_TARGET) $(CFLAGS) -c web.c -o web.o

web.elf: $(OBJS)
	$(CC_TARGET) $(LDFLAGS) -T $(LDSCRIPT) -Wl,-Map,web.map -o web.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble web.elf > web.dasm

# ZEXE, not a raw --pad-to binary. This app has a sizeable .bss -- the
# parser context alone is nearly 6KB and the block buffer for a screen
# is larger -- and the old format wrote .bss out as literal zeros, so
# all of it would be read off the SD card on every launch. The header
# carries it as a number instead. See sw/common/zexec.h.
web.bin: web.elf
	@EDATA=$$($(PREFIX)nm web.elf | awk '$$3=="_edata"{print "0x"$$1}'); \
	END=$$($(PREFIX)nm web.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	$(PREFIX)objcopy -O binary web.elf web.data; \
	python3 ../../../tools/mkexec.py web.data web.bin $$(($$END - $$EDATA)); \
	rm -f web.data

# -- host builds, with the host's own compiler, not $(PREFIX)gcc --

# The HOST compiler, kept separate from $(CC_TARGET) above. Using the
# same variable for both is how a "host" test ends up built for RISC-V
# and silently not run.
HOSTCC    ?= cc
HOSTFLAGS ?= --std=gnu99 -Wall -Wextra -O1 -g
COMMON    = ../../common

BUILD    ?= build

# Pages to run the html invariants over, in addition to the built-in
# synthetic ones. Any real page will do; the ones worth keeping around
# are the ugly ones.
PAGES    ?=

DOC      ?= tests/sample.html
SCROLL   ?= 0
SCALE    ?= 2
OUT      ?= /tmp/web.pbm

# zrender.h supplies software pixel primitives. zgfx.c is deliberately
# NOT in this list: it programs the GPU rasterizer through MMIO, which
# on a build machine writes into unmapped memory and draws nothing.
RENDER_SRC = $(COMMON)/zwin.c $(COMMON)/zwidget.c $(COMMON)/zfont_data.c \
             $(COMMON)/zobj.c $(COMMON)/zeitlos.c

.PHONY: web test test-tls test-asan render clean

TLS_HOST_SRC = tls.c tls_crypto.c ../../common/zsha256.c \
               ../../ext/monocypher/monocypher.c

test: $(BUILD)/test_url $(BUILD)/test_http $(BUILD)/test_html \
      $(BUILD)/test_layout $(BUILD)/test_page $(BUILD)/test_tls_crypto \
      $(BUILD)/test_x509 $(BUILD)/test_rsa \
      $(BUILD)/test_ecdsa $(BUILD)/test_ecdsa_hw $(BUILD)/test_solinas \
      $(BUILD)/test_toolbar \
      $(BUILD)/test_verify \
      $(BUILD)/test_sha384
	@echo "--- url ---"    && $(BUILD)/test_url
	@echo "--- toolbar ---" && $(BUILD)/test_toolbar
	@echo "--- http ---"   && $(BUILD)/test_http
	@echo "--- html ---"   && $(BUILD)/test_html $(PAGES)
	@echo "--- layout ---" && $(BUILD)/test_layout
	@echo "--- page ---"   && $(BUILD)/test_page $(PAGES)
	@echo "--- tls crypto ---" && $(BUILD)/test_tls_crypto
	@echo "--- x509 ---"    && $(BUILD)/test_x509
	@echo "--- rsa ---"     && $(BUILD)/test_rsa
	@echo "--- ecdsa ---"   && $(BUILD)/test_ecdsa
	@echo "--- ecdsa hw ---" && $(BUILD)/test_ecdsa_hw
	@echo "--- solinas ---" && $(BUILD)/test_solinas
	@echo "--- sha384 ---"  && $(BUILD)/test_sha384
	@echo "--- verify ---"  && $(BUILD)/test_verify $(BUILD)/test_rsa
	@echo "(for the live handshake against OpenSSL: make test-tls)"

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/test_url: tests/test_url.c url.c url.h | $(BUILD)
	$(HOSTCC) $(HOSTFLAGS) -o $@ tests/test_url.c url.c

$(BUILD)/test_http: tests/test_http.c http.c url.c http.h url.h \
		../../common/zinflate.c | $(BUILD)
	$(HOSTCC) $(HOSTFLAGS) -I ../../common -o $@ tests/test_http.c \
		http.c url.c ../../common/zinflate.c

$(BUILD)/test_html: tests/test_html.c html.c uni.c html.h uni.h | $(BUILD)
	$(HOSTCC) $(HOSTFLAGS) -o $@ tests/test_html.c html.c uni.c

# The layout tests and the renderer link against zrender.h, which maps
# a fake framebuffer with MAP_FIXED_NOREPLACE at the address zgfx.h
# expects. That is Linux on x86-64 only; both exit 77 (the automake
# convention for "skipped") elsewhere rather than failing.
$(BUILD)/test_layout: tests/test_layout.c layout.c html.c uni.c | $(BUILD)
	$(HOSTCC) $(HOSTFLAGS) -I $(COMMON) -o $@ \
		tests/test_layout.c layout.c html.c uni.c $(RENDER_SRC)

$(BUILD)/test_page: tests/test_page.c page.c layout.c html.c uni.c | $(BUILD)
	$(HOSTCC) $(HOSTFLAGS) -I $(COMMON) -o $@ \
		tests/test_page.c page.c layout.c html.c uni.c $(RENDER_SRC)

$(BUILD)/test_tls_crypto: tests/test_tls_crypto.c tls_crypto.c \
		tests/vectors.h | $(BUILD)
	$(HOSTCC) $(HOSTFLAGS) -o $@ tests/test_tls_crypto.c tls_crypto.c \
		../../common/zsha256.c ../../ext/monocypher/monocypher.c

$(BUILD)/test_rsa: tests/test_rsa.c rsa.c bignum.c tests/rsa_vectors.h \
		| $(BUILD)
	$(HOSTCC) $(HOSTFLAGS) -o $@ tests/test_rsa.c rsa.c bignum.c \
		../../common/zsha256.c

MONO = ../../ext/monocypher/monocypher.c \
       ../../ext/monocypher/monocypher-ed25519.c

# The same vectors through the HARDWARE path, with rtl/montmul.v
# replaced by a software model (EC_HW_SIM in ecdsa.c).
#
# It does not test the block -- rtl/tests/tb_montmul.v does that. It
# tests everything AROUND the block: the zero-padding to the block's
# width, the Montgomery representation, and reloading the modulus when
# the curve changes. Each of those has already produced a bug that
# only hardware could find, and two of them were found here instead.
$(BUILD)/test_ecdsa_hw: tests/test_ecdsa.c ecdsa.c tests/ecdsa_vectors.h \
		| $(BUILD)
	$(HOSTCC) $(HOSTFLAGS) -DEC_HW -DEC_HW_SIM -I ../../common \
		-o $@ tests/test_ecdsa.c ecdsa.c

$(BUILD)/test_solinas: tests/test_solinas.c ecdsa.c \
		tests/solinas_vectors.h | $(BUILD)
	$(HOSTCC) $(HOSTFLAGS) -o $@ tests/test_solinas.c

$(BUILD)/test_toolbar: tests/test_toolbar.c toolbar.c | $(BUILD)
	$(HOSTCC) $(HOSTFLAGS) -o $@ tests/test_toolbar.c toolbar.c

$(BUILD)/test_ecdsa: tests/test_ecdsa.c ecdsa.c tests/ecdsa_vectors.h \
		| $(BUILD)
	$(HOSTCC) $(HOSTFLAGS) -o $@ tests/test_ecdsa.c ecdsa.c

$(BUILD)/test_sha384: tests/test_sha384.c sha384.c | $(BUILD)
	$(HOSTCC) $(HOSTFLAGS) -o $@ tests/test_sha384.c sha384.c $(MONO)

# Runs from the app directory, because the certificate corpus is
# looked up as tests/chain/*.der.
$(BUILD)/test_verify: tests/test_verify.c verify.c x509.c der.c rsa.c \
		bignum.c ecdsa.c sha384.c | $(BUILD)
	$(HOSTCC) $(HOSTFLAGS) -o $@ tests/test_verify.c verify.c x509.c \
		der.c rsa.c bignum.c ecdsa.c sha384.c ../../common/zsha256.c $(MONO)

$(BUILD)/test_x509: tests/test_x509.c der.c x509.c | $(BUILD)
	$(HOSTCC) $(HOSTFLAGS) -o $@ tests/test_x509.c der.c x509.c

# The same tests under AddressSanitizer and UBSan.
#
# Worth a separate target because of what test_x509.c does at the end:
# it corrupts one byte at a time across every certificate in the
# corpus and parses each result, which is a few thousand deliberately
# malformed inputs. Plain, that only catches a parser that crashes or
# hangs. Under ASan it catches every read past the end of a buffer,
# which is the entire class of bug that matters in a certificate
# parser -- and it costs a second.
test-asan: | $(BUILD)
	$(HOSTCC) --std=gnu99 -g -fsanitize=address,undefined -o \
		$(BUILD)/test_x509_asan tests/test_x509.c der.c x509.c
	$(BUILD)/test_x509_asan
	$(HOSTCC) --std=gnu99 -g -fsanitize=address,undefined -o \
		$(BUILD)/test_rsa_asan tests/test_rsa.c rsa.c bignum.c \
		../../common/zsha256.c
	$(BUILD)/test_rsa_asan
	$(HOSTCC) --std=gnu99 -g -fsanitize=address,undefined -o \
		$(BUILD)/test_verify_asan tests/test_verify.c verify.c x509.c \
		der.c rsa.c bignum.c ecdsa.c sha384.c ../../common/zsha256.c \
		$(MONO)
	$(BUILD)/test_verify_asan
	$(HOSTCC) --std=gnu99 -g -fsanitize=address,undefined -I $(COMMON) -o \
		$(BUILD)/test_html_asan tests/test_html.c html.c uni.c
	$(BUILD)/test_html_asan $(PAGES)

# A LIVE handshake against OpenSSL, on localhost.
#
# Separate from `test` because it needs python3, the `cryptography`
# module and the openssl binary, and a test suite that cannot run
# without a network stack and a certificate toolchain is one people
# stop running. Everything it checks that CAN be checked offline --
# the key schedule, the AEAD, the label construction -- is in
# test_tls_crypto and runs unconditionally.
#
# Two things are being varied. The CHUNK SIZE, because a record spans
# several TCP segments on the real transport (net's MSS is 536), so a
# record layer that only works when whole records arrive at once
# passes here and fails on hardware. And the BODY SIZE, which
# tls_server.py defaults to 200KB: the first version of this test
# served 11 bytes, which fits in a single TLS record and therefore
# never exercised record framing at all -- while the app was failing
# part way through a real page.
test-tls: $(BUILD)/test_tls
	@for ch in 0 1 7 536 1024; do \
		port=$$((44400 + $$ch)); \
		python3 tests/tls_server.py $$port \
			--cert-der $(BUILD)/server$$ch.der \
			> $(BUILD)/tls_server.log 2>&1 & \
		sleep 2; \
		$(BUILD)/test_tls $$port $$ch verify $(BUILD)/server$$ch.der \
			|| exit 1; \
	done
	@echo "verified handshake ok at every chunk size"
	@# The refusals. These are the runs that prove verification is
	@# actually consulted rather than merely linked in -- a client
	@# that skipped it would pass every test above.
	@for mode in none wronghost; do \
		python3 tests/tls_server.py 44399 \
			--cert-der $(BUILD)/server_neg.der \
			> $(BUILD)/tls_server.log 2>&1 & \
		sleep 2; \
		$(BUILD)/test_tls 44399 0 $$mode $(BUILD)/server_neg.der \
			|| exit 1; \
	done

$(BUILD)/test_tls: tests/test_tls.c tls.c tls_crypto.c verify.c x509.c \
		der.c rsa.c bignum.c ecdsa.c sha384.c | $(BUILD)
	$(HOSTCC) $(HOSTFLAGS) -DTLS_HOST_TEST -o $@ tests/test_tls.c \
		tls.c tls_crypto.c verify.c x509.c der.c rsa.c bignum.c ecdsa.c \
		sha384.c ../../common/zsha256.c $(MONO)

$(BUILD)/render: tests/render.c layout.c html.c uni.c | $(BUILD)
	$(HOSTCC) $(HOSTFLAGS) -I $(COMMON) -o $@ \
		tests/render.c layout.c html.c uni.c $(RENDER_SRC)

render: $(BUILD)/render
	$(BUILD)/render $(DOC) $(SCROLL) $(SCALE) $(OUT)
	@echo "wrote $(OUT) -- view it, do not just read the log"

clean:
	rm -rf $(BUILD)
	rm -f web.elf web.bin web.data web.asm web.dasm web.map *.o *.d
	rm -f .web_config_selected

# Header dependency tracking -- CFLAGS carries -MD, and without
# including the .d files an edit to a header rebuilds nothing. See
# sw/apps/net/Makefile for the longer version of why that matters.
-include $(wildcard *.d)

# -- the trust store --------------------------------------------------
#
# See roots/README.md. The PEM is VENDORED, not downloaded: the store
# is the trust anchor, and a build that fetches it can silently change
# what every machine believes.
#
# ROOTS_SHA256 pins the input. A changed PEM fails the build rather
# than quietly altering what is trusted -- updating it is meant to be
# a commit somebody reviews.
#
# ROOTS_PEM defaults to the vendored file and can be pointed at a
# build machine's own store for a local build:
#
#   make roots ROOTS_PEM=/etc/ssl/certs/ca-certificates.crt ROOTS_SHA256=
#
ROOTS_PEM    ?= roots/cacert.pem
ROOTS_SHA256 ?=

.PHONY: roots test-roots
roots: roots.der

# Not part of `make test`: it needs a PEM, and a suite that cannot run
# without one is a suite people stop running.
test-roots: roots.der
	$(HOSTCC) $(HOSTFLAGS) -o $(BUILD)/test_roots tests/test_roots.c \
		x509.c der.c ../../common/zsha256.c
	$(BUILD)/test_roots roots.der

roots.der: $(ROOTS_PEM) ../../../tools/mkroots.py
	python3 ../../../tools/mkroots.py $(ROOTS_PEM) -o $@ \
		$(if $(ROOTS_SHA256),--expect-sha256 $(ROOTS_SHA256),)

# -- every source is linked ------------------------------------------
#
# `toolbar.c` was written, tested, and left out of OBJS. Nothing
# noticed until the target linker did, because the host tests build
# their own file lists and a dry run of this Makefile happily compiles
# whatever it is told to.
#
# This compares the .c files in this directory against the object list
# and fails if any is missing. It is cheap and it catches the one
# mistake the rest of the build cannot see.
.PHONY: check-sources
check-sources:
	@missing=""; \
	for c in *.c; do \
		case "$$c" in render.c|*_test.c) continue;; esac; \
		o=`echo $$c | sed 's/\.c$$/.o/'`; \
		case " $(OBJS) " in *" $$o "*) ;; *) missing="$$missing $$c";; esac; \
	done; \
	if [ -n "$$missing" ]; then \
		echo "check-sources: not in OBJS:$$missing"; exit 1; \
	else echo "check-sources: every source is linked"; fi
