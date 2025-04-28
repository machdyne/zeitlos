
PREFIX = /opt/riscv32i/bin/riscv32-unknown-elf-
ARCH = rv32i
CC = $(PREFIX)gcc
AS = $(PREFIX)as
ASFLAGS = -march=$(ARCH) -mabi=ilp32
CFLAGS = -fPIC --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=ilp32
LDFLAGS = -fPIC -nostartfiles
LDSCRIPT = ../../common/riscv-app.ld

OBJS = crt0.o zeitlos.o hello.o

hello: hello.elf hello.bin

crt0.o:
	$(AS) $(ASFLAGS) -o crt0.o ../../common/crt0.S

zeitlos.o:
	$(CC) -c ../../common/zeitlos.c -o zeitlos.o

hello.o:
	$(CC) $(CFLAGS) -c hello.c -o hello.o
	$(CC) $(CFLAGS) -c hello.c -S -o hello.asm

hello.elf: $(OBJS)
	$(CC) $(LDFLAGS) -Wl,-T$(LDSCRIPT),-Map,hello.map -o hello.elf $(OBJS) 
	$(PREFIX)objdump -S --disassemble hello.elf > hello.dasm

hello.bin:
	$(PREFIX)objcopy -O binary hello.elf hello.bin

clean:
	rm -f hello.elf hello.bin hello.asm hello.dasm hello.map *.o *.d

.PHONY: hello clean

