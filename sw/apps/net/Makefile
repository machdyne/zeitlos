
PREFIX = /opt/riscv32i/bin/riscv32-unknown-elf-
ARCH = rv32i
CC = $(PREFIX)gcc
AS = $(PREFIX)as
ASFLAGS = -march=$(ARCH) -mabi=ilp32
CFLAGS = --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=ilp32
LDFLAGS = -march=$(ARCH) -mabi=ilp32
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

# DHCP (dhcp.c) on by default; set NET_DHCP=0 to disable it entirely
# and always use the static NET_STATIC_* config below -- e.g. for a
# board with no DHCP server on its network, or any setup where a
# fixed address is wanted regardless of what a server might offer.
# With DHCP left on (the default), NET_STATIC_* is still meaningful:
# it's the fallback net.c uses if no DHCP server answers within
# dhcp_acquire()'s retry budget (see dhcp.c/docs/networking.md) --
# same three values, two different roles depending on NET_DHCP.
NET_DHCP ?= 1
CFLAGS += -DNET_DHCP=$(NET_DHCP)

# accepted either as a dotted quad ("192.168.1.50") or already as a
# 0x-prefixed hex literal ("0xC0A80132") -- either way, converted here
# to a single hex constant (one byte per octet) since that's what a
# C -D define actually needs to end up as.
NET_STATIC_IP      ?= 192.168.178.230
NET_STATIC_NETMASK ?= 255.255.255.0
NET_STATIC_GATEWAY ?= 192.168.178.1

# nameserver override -- 0.0.0.0 (the default) means "no override,
# just use whatever DHCP provides (option 6), or none at all if DHCP
# is off/doesn't provide one" -- see net.c's own OUR_DNS comment and
# docs/networking.md's "DNS client" section. Unlike NET_STATIC_IP/
# NETMASK/GATEWAY above, this is NOT a DHCP-failure fallback: if set,
# it always wins over whatever DHCP said, even on a successful lease.
NET_STATIC_DNS     ?= 0.0.0.0

dottedquad_to_hex = $(shell printf '%s' "$(1)" | grep -qE '^0[xX]' && \
	printf '%s' "$(1)" || \
	printf '0x%02X%02X%02X%02X' $$(printf '%s' "$(1)" | tr '.' ' '))

CFLAGS += -DNET_STATIC_IP=$(call dottedquad_to_hex,$(NET_STATIC_IP))
CFLAGS += -DNET_STATIC_NETMASK=$(call dottedquad_to_hex,$(NET_STATIC_NETMASK))
CFLAGS += -DNET_STATIC_GATEWAY=$(call dottedquad_to_hex,$(NET_STATIC_GATEWAY))
CFLAGS += -DNET_STATIC_DNS=$(call dottedquad_to_hex,$(NET_STATIC_DNS))

ifeq ($(NET_DHCP), 1)
DHCP_OBJ = dhcp.o
else
DHCP_OBJ =
endif

# dns.c (sw/apps/net/dns.c) is always built in, independent of
# NET_DHCP -- it only needs SOME nameserver configured (DHCP-provided
# or NET_STATIC_DNS), not DHCP itself, so unlike dhcp.o there's no
# "off" switch for it tied to NET_DHCP.
OBJS = zeitlos.o zobj.o zstream.o zport.o $(PHY_OBJ) eth.o arp.o ip.o udp.o $(DHCP_OBJ) dns.o tcp.o telnet.o tftp.o net.o

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
#
# NET_DHCP/NET_STATIC_* are the same class of bug again, third time:
# they only affect net.c's CFLAGS (and, for NET_DHCP=0, which objects
# get linked at all), not net.c's own file content, so switching e.g.
# `make NET_DHCP=0` after a previous `make NET_DHCP=1` build wouldn't
# otherwise trigger a rebuild of the stale net.o. .net_config_selected
# follows the exact same pattern as .net_phy_selected, just tracking
# a different set of values (now including NET_STATIC_DNS too).

FORCE:

.net_phy_selected: FORCE
	@if [ ! -f $@ ] || [ "$$(cat $@ 2>/dev/null)" != "$(NET_PHY)" ]; then \
		echo "$(NET_PHY)" > $@; \
	fi

.net_config_selected: FORCE
	@cur="$(NET_DHCP) $(NET_STATIC_IP) $(NET_STATIC_NETMASK) $(NET_STATIC_GATEWAY) $(NET_STATIC_DNS)"; \
	if [ ! -f $@ ] || [ "$$(cat $@ 2>/dev/null)" != "$$cur" ]; then \
		echo "$$cur" > $@; \
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

dhcp.o: dhcp.c dhcp.h udp.h ip.h eth.h
	$(CC) $(CFLAGS) -c dhcp.c -o dhcp.o

dns.o: dns.c dns.h udp.h ../../common/znet.h
	$(CC) $(CFLAGS) -c dns.c -o dns.o

tcp.o: tcp.c tcp.h
	$(CC) $(CFLAGS) -c tcp.c -o tcp.o

telnet.o: telnet.c telnet.h tcp.h
	$(CC) $(CFLAGS) -c telnet.c -o telnet.o

tftp.o: tftp.c tftp.h
	$(CC) $(CFLAGS) -c tftp.c -o tftp.o

net.o: net.c net_phy.h .net_phy_selected .net_config_selected ../../common/zsoc.h dhcp.h dns.h
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
	rm -f net.elf net.bin net.asm net.dasm net.map *.o *.d .net_phy_selected .net_config_selected

.PHONY: net clean FORCE
