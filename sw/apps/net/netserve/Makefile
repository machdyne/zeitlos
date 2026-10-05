# sw/apps/net/netserve -- build rules are shared: sw/common/app.mk, docs/build.md.

APP = netserve
APP_LISTING = 0
# netserve -- network servers (SSH, telnet, HTTP, echo). docs/netserve.md.
# Lives under sw/apps/net because it is net's other half: net accepts
# the connections (relay.c) and this serves them. It shares no code
# with net today; the SSH server (a later phase) will compile
# ../ssh/ssh_wire.c and friends from here, in place.

# zport's own errors are printed (no -DZPORT_NO_PRINTF): a failed
# allocation in z_port_send() is otherwise silent, and looks exactly
# like a hang.

# SSH: the server engine and what it shares with net's client, compiled
# in place from ../ssh and sw/ext/monocypher -- no copies.
MONO = ../../../ext/monocypher
SSH_OBJS = ssh_server.o ssh_wire.o ssh_crypto.o zsha256.o monocypher.o monocypher-ed25519.o z25519.o zrng.o \
	authkeys.o rsa.o bignum.o

OBJS = zeitlos.o zobj.o zport.o zcfg.o zfsapp.o $(SSH_OBJS) netserve.o

ssh_server.o: ../ssh/ssh_server.c ../ssh/ssh_server.h ../ssh/ssh_crypto.h ../ssh/ssh_wire.h
	$(CC) $(CFLAGS) -c ../ssh/ssh_server.c -o ssh_server.o
ssh_wire.o: ../ssh/ssh_wire.c ../ssh/ssh_wire.h
	$(CC) $(CFLAGS) -c ../ssh/ssh_wire.c -o ssh_wire.o
ssh_crypto.o: ../ssh/ssh_crypto.c ../ssh/ssh_crypto.h
	$(CC) $(CFLAGS) -c ../ssh/ssh_crypto.c -o ssh_crypto.o
monocypher.o: $(MONO)/monocypher.c $(MONO)/monocypher.h
	$(CC) $(CFLAGS) -c $(MONO)/monocypher.c -o monocypher.o

# X25519 and Ed25519 checks on montmul's register file (docs/z25519.md)
z25519.o: ../../../common/z25519.c ../../../common/z25519.h
	$(CC) $(CFLAGS) -c ../../../common/z25519.c -o z25519.o
monocypher-ed25519.o: $(MONO)/monocypher-ed25519.c $(MONO)/monocypher-ed25519.h
	$(CC) $(CFLAGS) -I$(MONO) -c $(MONO)/monocypher-ed25519.c -o monocypher-ed25519.o
authkeys.o: authkeys.c authkeys.h
	$(CC) $(CFLAGS) -c authkeys.c -o authkeys.o
# RSA verification, from web (which checks certificates with it)
rsa.o: ../../web/rsa.c ../../web/rsa.h
	$(CC) $(CFLAGS) -c ../../web/rsa.c -o rsa.o
bignum.o: ../../web/bignum.c ../../web/bignum.h
	$(CC) $(CFLAGS) -c ../../web/bignum.c -o bignum.o

# Host tests: tests/test_netserve.c runs the real netserve.c against a
# scripted net, backend and kernel.
HOSTCC ?= cc
# The SSH engine and its crypto, for the host tests. zrng.c is not among
# them: it reads the TRNG's registers; the tests bring their own.
TEST_SSH = ../ssh/ssh_server.c ../ssh/ssh_wire.c ../ssh/ssh_crypto.c ../../../common/zsha256.c \
	$(MONO)/monocypher.c $(MONO)/monocypher-ed25519.c ../../../common/z25519.c \
	authkeys.c ../../web/rsa.c ../../web/bignum.c
.PHONY: test
test:
	$(HOSTCC) --std=gnu99 -O2 -Wall -o /tmp/test_authkeys tests/test_authkeys.c $(TEST_SSH)
	/tmp/test_authkeys
	$(HOSTCC) --std=gnu99 -O2 -w -I../ssh -o /tmp/test_ssh_server ../ssh/tests/test_ssh_server.c \
		$(TEST_SSH)
	/tmp/test_ssh_server
	$(HOSTCC) --std=gnu99 -Wall -no-pie -I../../../common -o /tmp/test_netserve \
		tests/test_netserve.c tests/fake_fs.c $(TEST_SSH) ../ssh/ssh_proto.c ../../../common/zobj.c \
		../../../common/zport.c ../../../common/zeitlos.c ../../../common/zcfg.c
	/tmp/test_netserve || [ $$? -eq 77 ]
	$(HOSTCC) --std=gnu99 -w -no-pie -I../../../common -I.. -o /tmp/test_e2e \
		tests/test_e2e.c tests/ns_harness.c tests/fake_fs.c $(TEST_SSH) ../tcp.c ../relay.c ../../../common/zobj.c \
		../../../common/zport.c ../../../common/zeitlos.c ../../../common/zcfg.c
	/tmp/test_e2e || [ $$? -eq 77 ]

# 64KB. SSH engines on the heap, about 10KB each, two by default
# and four at most, plus eight sends in flight per session each way.
APP_TIER = LARGE

include ../../../common/app.mk
