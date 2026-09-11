# posix shell tests, built with the HOST compiler.
CC ?= cc
CFLAGS ?= -O1 -g -std=gnu99 -Wall -Wextra -Werror -Wno-unused-parameter -I.. -I../../common

run: px_shell_test px_case_test
	@./run.sh

px_shell_test: run_sh.c host_fs.c ../sh.c ../vfs.c ../posix.h
	$(CC) $(CFLAGS) -DPX_HOST_TEST=1 -o $@ run_sh.c host_fs.c ../sh.c ../vfs.c

px_case_test: run_cases.c host_fs.c ../sh.c ../vfs.c ../posix.h
	$(CC) $(CFLAGS) -DPX_HOST_TEST=1 -o $@ run_cases.c host_fs.c ../sh.c ../vfs.c

clean:
	rm -f px_shell_test px_case_test

.PHONY: run clean
