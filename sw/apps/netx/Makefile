# sw/apps/netx -- netx, the full net (docs/netx.md): sw/apps/net's
# sources built with -DNETX, on the card. Flash net is the same sources
# without it.
#
# Built here, not in sw/apps/net, so that its objects never mix with
# flash net's: net's sources are linked into this directory (the same
# depth, so their relative includes hold) and net's own Makefile runs
# with NETX=1. The links are made at each build and are not checked in.

.PHONY: all clean links test

all: links
	$(MAKE) -f ../net/Makefile NETX=1

links:
	@for f in ../net/*.c ../net/*.h; do \
		b=$$(basename $$f); [ -e $$b ] || ln -s $$f $$b; \
	done; [ -e ssh ] || ln -s ../net/ssh ssh

clean:
	-$(MAKE) -f ../net/Makefile NETX=1 clean
	find . -maxdepth 1 -type l -delete
