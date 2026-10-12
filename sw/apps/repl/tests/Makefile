# Host test for repl's (help) and (apropos): zapi_doc.c against the real
# zapi.txt, made here by tools/gen_zapi.py, which also checks that every
# procedure repl has is in it. --run-examples runs the Scheme-language
# examples through a desktop build of ms and compares their values.
#
#   make -C sw/apps/repl/tests
#
# Needs the ms submodule (git submodule update --init sw/ext/ms).
ROOT = ../../../..
CFLAGS = -std=gnu99 -O1 -g -Wall -Wextra -fsanitize=address,undefined

all:
	cc -O1 -o /tmp/zapi_ms $(ROOT)/sw/ext/ms/ms.c -lm
	python3 $(ROOT)/tools/gen_zapi.py --txt /tmp/zapi_test.txt --run-examples /tmp/zapi_ms
	cc $(CFLAGS) -o /tmp/test_zapi_doc test_zapi_doc.c ../zapi_doc.c
	/tmp/test_zapi_doc /tmp/zapi_test.txt
