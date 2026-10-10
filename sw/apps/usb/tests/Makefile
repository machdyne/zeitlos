# Host test for the `usb` process around the stack: see test_usb.c. The
# stack itself is tested against the gateware by the co-simulation
# (`make test_usb_cosim` and the rest, at the top level).

default: test_usb
CC ?= cc
COMMON = ../../../common
CFLAGS = -std=gnu99 -Wall -O1 -g \
	-fsanitize=alignment,address,undefined -fno-sanitize-recover=all \
	-I$(COMMON) -DUSB_HOST_TEST -Wno-unused-result

test_usb: test_usb.c ../usb.c ../usbh.h $(COMMON)/zusbc.c $(COMMON)/zusbproto.h $(COMMON)/zusbnet.h $(COMMON)/zobj.c
	$(CC) $(CFLAGS) -o $@ test_usb.c ../usb.c $(COMMON)/zusbc.c $(COMMON)/zobj.c
	./test_usb

clean:
	rm -f test_usb
