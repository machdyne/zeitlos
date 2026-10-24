# Host test for the filesystem driver: see test_fs.c.
#
#   make -C sw/apps/fs/tests run      build and run

default: test_fs test_fs_kernel test_fsvol
CC ?= cc
COMMON = ../../../common
CFLAGS = -std=gnu99 -Wall -O1 -g \
	-fsanitize=alignment,address,undefined -fno-sanitize-recover=all \
	-I$(COMMON) -I../../net -DFS_HOST_TEST -DZFSAPP_DIRECT '-DFS_IN_WINDOW(d,n,b,s)=1'

test_fs: test_fs.c nfs_fake.h ../../net/nfs.c ../fs.c ../fsvol.h $(COMMON)/zfsc.c $(COMMON)/zfsapp.c $(COMMON)/zposix.c ../fatfs/ff.c ../fatfs/ffunicode.c
	$(CC) $(CFLAGS) -DZPOSIX_HOST_TEST -o $@ test_fs.c ../fs.c $(COMMON)/zfsc.c $(COMMON)/zfsapp.c $(COMMON)/zposix.c ../fatfs/ff.c ../fatfs/ffunicode.c

# The same, with the client built as the kernel builds it (sw/os/kfs.c).
test_fs_kernel: test_fs.c nfs_fake.h ../../net/nfs.c ../fs.c ../fsvol.h $(COMMON)/zfsc.c $(COMMON)/zfsapp.c $(COMMON)/zposix.c ../fatfs/ff.c ../fatfs/ffunicode.c
	$(CC) $(CFLAGS) -DZFSC_KERNEL -DZPOSIX_HOST_TEST -o $@ test_fs.c ../fs.c $(COMMON)/zfsc.c $(COMMON)/zfsapp.c $(COMMON)/zposix.c ../fatfs/ff.c ../fatfs/ffunicode.c

# Rename's checks and FAT timestamps (fsvol.h), on FatFs itself.
test_fsvol: test_fsvol.c ../fsvol.h ../fatfs/ff.c ../fatfs/ffunicode.c
	$(CC) -std=gnu99 -Wall -I$(COMMON) -o $@ test_fsvol.c ../fatfs/ff.c ../fatfs/ffunicode.c

all: test_fs test_fs_kernel test_fsvol

# build and run all three (tools/run_tests.py runs this)
run: test_fs test_fs_kernel test_fsvol
	./test_fs
	./test_fs_kernel
	./test_fsvol

.PHONY: default all run clean

clean:
	rm -f test_fs test_fs_kernel test_fsvol
