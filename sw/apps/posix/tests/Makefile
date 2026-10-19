# posix shell tests, built with the HOST compiler.
CC ?= cc
CFLAGS ?= -O1 -g -std=gnu99 -Wall -Wextra -Werror -Wno-unused-parameter -I.. -I../../../common

run: px_shell_test px_case_test
	@./run.sh

GZ = ../../../common/zgz.c ../../../common/zinflate.c ../../../common/zfsutil.c
# unzip and tar: the formats library's archive code, as the device links it
ZF = ../../../common/formats/zf_core.c ../../../common/formats/zf_xml.c \
	../../../common/formats/zf_zip.c ../../../common/formats/zf_arch.c
SRC = host_fs.c ../sh.c ../vfs.c ../tools.c ../tree.c $(GZ) $(ZF) ../../../common/zwalk.c

px_shell_test: run_sh.c $(SRC) ../posix.h
	$(CC) $(CFLAGS) -DPX_HOST_TEST=1 -o $@ run_sh.c $(SRC)

px_case_test: run_cases.c $(SRC) ../posix.h
	$(CC) $(CFLAGS) -DPX_HOST_TEST=1 -o $@ run_cases.c $(SRC)

clean:
	rm -f px_shell_test px_case_test

.PHONY: run clean
