
PREFIX = /opt/riscv32i/bin/riscv32-unknown-elf-
ARCH = rv32i
CC = $(PREFIX)gcc
AS = $(PREFIX)as
ASFLAGS = -march=$(ARCH) -mabi=ilp32
CFLAGS = --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=ilp32
LDFLAGS =
LDSCRIPT = ../../common/riscv-app.ld

# which NIC driver to build in -- ENC28J60 (SPI PMOD, most boards) or
# RMII (rtl/ethmac_rmii.v, mozart_ml1 only). See net_phy.h for why
# this is a build-time choice, not a runtime probe.
NET_PHY ?= ENC28J60
#NET_PHY ?= RMII
CFLAGS += -DNET_PHY_$(NET_PHY)

ifeq ($(NET_PHY), RMII)
PHY_OBJ = rmii_eth.o
else
PHY_OBJ = enc28j60.o
endif

OBJS = zeitlos.o zobj.o zstream.o zport.o $(PHY_OBJ) eth.o arp.o ip.o udp.o tcp.o telnet.o tftp.o net.o

net: net.elf net.bin

# each rule below lists its own .c file as a prerequisite -- without
# this, `make` has no way to know a source file changed (it was only
# checking whether the .o file existed at all), so it would happily
# keep linking a stale .o built from an older version of that file
# after any edit. bit us for real with sw/os/fs/fs.c during this
# session's zstream/TFTP work -- see docs/networking.md.
#
# NET_PHY itself is a second, different way the same class of
# staleness bug can happen: switching it changes CFLAGS and which
# driver .o gets linked, not any .c file's contents, so a plain .c
# prerequisite wouldn't notice `make NET_PHY=RMII` after a previous
# `make NET_PHY=ENC28J60` and would happily link a stale eth.o/net.o
# still compiled with the OTHER driver's -DNET_PHY_* -- silently
# building for the wrong board. .net_phy_selected below is a real
# file whose mtime only changes when NET_PHY's value actually does
# (FORCE makes its own recipe re-run the check every build; the
# recipe itself only rewrites the file, bumping its mtime, if the
# value differs from last time), and every NET_PHY-sensitive object
# depends on it.

FORCE:

.net_phy_selected: FORCE
	@if [ ! -f $@ ] || [ "$$(cat $@ 2>/dev/null)" != "$(NET_PHY)" ]; then \
		echo "$(NET_PHY)" > $@; \
	fi

zeitlos.o: ../../common/zeitlos.c ../../common/zeitlos.h
	$(CC) $(CFLAGS) -c ../../common/zeitlos.c -o zeitlos.o

zobj.o: ../../common/zobj.c ../../common/zobj.h
	$(CC) $(CFLAGS) -c ../../common/zobj.c -o zobj.o

zstream.o: ../../common/zstream.c ../../common/zstream.h
	$(CC) $(CFLAGS) -c ../../common/zstream.c -o zstream.o

zport.o: ../../common/zport.c ../../common/zport.h
	$(CC) $(CFLAGS) -c ../../common/zport.c -o zport.o

enc28j60.o: enc28j60.c enc28j60.h .net_phy_selected
	$(CC) $(CFLAGS) -c enc28j60.c -o enc28j60.o

rmii_eth.o: rmii_eth.c rmii_eth.h .net_phy_selected
	$(CC) $(CFLAGS) -c rmii_eth.c -o rmii_eth.o

eth.o: eth.c eth.h net_phy.h .net_phy_selected
	$(CC) $(CFLAGS) -c eth.c -o eth.o

arp.o: arp.c arp.h
	$(CC) $(CFLAGS) -c arp.c -o arp.o

ip.o: ip.c ip.h
	$(CC) $(CFLAGS) -c ip.c -o ip.o

udp.o: udp.c udp.h
	$(CC) $(CFLAGS) -c udp.c -o udp.o

tcp.o: tcp.c tcp.h
	$(CC) $(CFLAGS) -c tcp.c -o tcp.o

telnet.o: telnet.c telnet.h tcp.h
	$(CC) $(CFLAGS) -c telnet.c -o telnet.o

tftp.o: tftp.c tftp.h
	$(CC) $(CFLAGS) -c tftp.c -o tftp.o

net.o: net.c net_phy.h .net_phy_selected ../../common/zsoc.h
	$(CC) $(CFLAGS) -c net.c -o net.o
	$(CC) $(CFLAGS) -c net.c -S -o net.asm

net.elf: $(OBJS)
	$(CC) $(LDFLAGS) -Wl,-T$(LDSCRIPT),-Map,net.map -o net.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble net.elf > net.dasm

net.bin: net.elf
	@END=$$($(PREFIX)nm net.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	echo "net.bin: padding to _end=$$END (includes .bss -- see docs/networking.md, \"objcopy truncation bug\")"; \
	$(PREFIX)objcopy -O binary --pad-to=$$END net.elf net.bin

clean:
	rm -f net.elf net.bin net.asm net.dasm net.map *.o *.d .net_phy_selected

.PHONY: net clean FORCE
