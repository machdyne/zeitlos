# Host-side test for gpu3d's STL loader. Builds with the SYSTEM gcc,
# not the RISC-V toolchain -- the point is to exercise the parser and
# the decimator on a workstation, where a failure is a printed
# assertion rather than a blank window on an FPGA.
#
# stl.c is the SAME source the app builds; -DSTL_HOST_TEST only swaps
# zfsapp.h's fs_* calls for the stdio shims in stl_host_stubs.h.

CFLAGS = -std=gnu99 -Wall -Wextra -O1 -DSTL_HOST_TEST -I. -I..

stl_test: stl_test.c ../stl.c ../stl.h ../model.h stl_host_stubs.h
	$(CC) $(CFLAGS) ../stl.c stl_test.c -o stl_test -lm

# A teapot-shaped test model. The argument is a detail factor tuned so
# the ASCII file lands near the ~400KB size this app was built to
# handle; the binary one is generated coarser-per-byte, so it is asked
# for a higher factor to reach a comparable size.
teapots: gen_teapot.py
	python3 gen_teapot.py 0.58 teapot.stl /dev/null
	python3 gen_teapot.py 1.28 /dev/null teapot_bin.stl

check: stl_test teapots
	./stl_test teapot.stl teapot_bin.stl

clean:
	rm -f stl_test teapot.stl teapot_bin.stl

.PHONY: teapots check clean
