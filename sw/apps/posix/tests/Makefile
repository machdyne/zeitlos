# posix shell tests, built with the HOST compiler.
CC ?= cc
CFLAGS ?= -O1 -g -std=gnu99 -Wall -Wextra -Werror -Wno-unused-parameter -I.. -I../../../common

run: px_shell_test px_case_test px_main_test
	@./run.sh

GZ = ../../../common/zgz.c ../../../common/zinflate.c ../../../common/zfsutil.c
# unzip and tar: the formats library's archive code, as the device links it
ZF = ../../../common/formats/zf_core.c ../../../common/formats/zf_xml.c \
	../../../common/formats/zf_zip.c ../../../common/formats/zf_arch.c
SRC = host_fs.c ../sh.c ../vfs.c ../tools.c ../tree.c ../job.c $(GZ) $(ZF) ../../../common/zwalk.c \
	../../../common/zco.c

px_shell_test: run_sh.c $(SRC) ../posix.h
	$(CC) $(CFLAGS) -DPX_HOST_TEST=1 -o $@ run_sh.c $(SRC)

px_case_test: run_cases.c $(SRC) ../posix.h
	$(CC) $(CFLAGS) -DPX_HOST_TEST=1 -o $@ run_cases.c $(SRC)

# The main loop (run_main.c includes ../main.c), with a fake kernel and
# terminals around it.
px_main_test: run_main.c ../main.c $(SRC) ../posix.h
	$(CC) $(CFLAGS) -DPX_HOST_TEST=1 -DPX_HOST_MAIN=1 -o $@ run_main.c $(SRC) ../../../common/zline.c

clean:
	rm -f px_shell_test px_case_test px_main_test

.PHONY: run clean
