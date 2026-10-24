# Host tests for the kernel (sw/os). `make test` here, or `make test` at
# the top of the tree for every suite (tools/run_tests.py, docs/build.md).
#
# Each recipe is the one in its test's header comment, run from the top
# of the tree. Binaries go to $(OUT).

R = ../../..
OUT ?= /tmp/zeitlos-os-tests
HOSTCC ?= cc
SAN = -fsanitize=address,undefined

.PHONY: test auth kvlog mem msg pidset proclist rescue zar zarcopy clean

test: auth kvlog mem msg pidset proclist rescue zar zarcopy

$(OUT):
	mkdir -p $(OUT)

auth: | $(OUT)
	cd $(R) && $(HOSTCC) -std=gnu99 -O2 -Wall -o $(OUT)/test_auth \
		sw/os/tests/test_auth.c sw/os/authcore.c sw/common/zsha256.c && $(OUT)/test_auth

kvlog: | $(OUT)
	cd $(R) && $(HOSTCC) -std=gnu99 -O2 -Wall -o $(OUT)/test_kvlog \
		sw/os/tests/test_kvlog.c sw/os/kvlog.c && $(OUT)/test_kvlog

mem: | $(OUT)
	cd $(R) && $(HOSTCC) -std=gnu99 -Wall -no-pie -I sw/os -I sw/common -o $(OUT)/test_mem \
		sw/os/tests/test_mem.c && $(OUT)/test_mem

msg: | $(OUT)
	cd $(R) && $(HOSTCC) -std=gnu99 -O2 -Wall -Wno-int-to-void-pointer-cast \
		-Wno-void-pointer-to-int-cast -I sw/common -o $(OUT)/test_msg \
		sw/os/tests/test_msg.c sw/common/zobj.c && $(OUT)/test_msg

# at several process-table sizes
pidset: | $(OUT)
	cd $(R) && for n in 32 64 100 128 256; do \
		$(HOSTCC) -std=gnu99 -Wall -DZ_PROCS_MAX=$$n -o $(OUT)/test_pidset \
			sw/os/tests/test_pidset.c && $(OUT)/test_pidset || exit 1; done

proclist: | $(OUT)
	cd $(R) && $(HOSTCC) -std=gnu99 -Wall -no-pie -I sw/os -I sw/common -o $(OUT)/test_proclist \
		sw/os/tests/test_proclist.c && $(OUT)/test_proclist

rescue: | $(OUT)
	cd $(R) && $(HOSTCC) -std=gnu99 -Wall -I sw/common -o $(OUT)/test_rescue \
		sw/os/tests/test_rescue.c sw/common/zfont_data.c && $(OUT)/test_rescue

# Needs a built wm (it compares wm as loaded with sw/apps/wm/wm.bin);
# skipped without one. The archive is the one docs/flash_apps.md makes.
zar: | $(OUT)
	@cd $(R) && if [ ! -f sw/apps/wm/wm.bin ]; then \
		echo "test_zar: skipped (build sw/apps/wm first)"; exit 0; fi; \
	python3 tools/mkzar.py $(OUT)/t.zar wm=sw/apps/wm/wm.bin net=README.md \
		docs/welcome.txt=docs/welcome.md docs/repl.txt=docs/boot.md \
		help/a/b.txt=LICENSE.md > /dev/null && \
	$(HOSTCC) -std=gnu99 -Wall -no-pie -DZAR_HOST_TEST -I sw/os -o $(OUT)/test_zar \
		sw/os/tests/test_zar.c sw/os/zar.c sw/common/zexec_lz4.c && $(OUT)/test_zar $(OUT)/t.zar

zarcopy: | $(OUT)
	cd $(R) && $(HOSTCC) -std=gnu99 -Wall -o $(OUT)/test_zarcopy \
		sw/os/tests/test_zarcopy.c && $(OUT)/test_zarcopy

clean:
	rm -rf $(OUT)
