# Upgrading the DFU bootloader

**Who this is for:** owners of a Lakritz or Obst who flash over USB
with `dfu-util` rather than over JTAG.

If you flash with `openFPGALoader` and a JTAG cable, you do not need
this. Use `zeitlos-<board>.img` and ignore the rest of this document.

---

## Why

The original DFU bootloader divides the 2 MB flash into two partitions:

```
BOOTPART   0x000000 – 0x0FFFFF   1024 KB   the bootloader itself
USERPART   0x100000 – 0x1FFFFF   1024 KB   your gateware
```

Zeitlos is 1303 KB — gateware, kernel, boot splash and the core
applications. It does not fit in 1024 KB, and no amount of trimming
gets it there while keeping a system that boots to a desktop without
an sdcard.

The original bootloader reserves a megabyte for itself and uses about an
eighth of it — the Lakritz image is 124,643 bytes and the Obst image
126,079. The *updated* bootloader takes 256 KB, twice what it needs,
and gives the rest to you:

```
BOOTPART   0x000000 – 0x03FFFF    256 KB   the bootloader itself
USERPART   0x040000 – 0x1FFFFF   1792 KB   your gateware
```

Nothing else changes. The same `dfu-util -a 0 -D` command flashes your
gateware afterwards, and any other project that fits in 1 MB still
fits — the user partition only got larger.

---

## Do you need it?

```
$ dfu-util -l
```

Look at the `alt=0` line:

```
Found DFU: [16d0:116d] ... alt=0, name="User Image (1792KB)", ...
```

- **`User Image (1792KB)`** — you already have the updated bootloader.
  Nothing to do.
- **`User Image` with no size** — you have the original. Continue.

The original bootloader does not put the partition size in the name at
all, so "no size shown" is itself the answer.

---

## Read this before you start

Writing the bootloader is the one operation on this board that can
leave it unable to enumerate over USB. Everything else is recoverable
over USB; this is not.

**It is not permanent damage.** The flash is a socketed MMOD module,
and reflashing it externally is straightforward if you have a
programmer. It is *inconvenient*, not fatal — see
[Recovery](#recovery) at the end, and read that section BEFORE you
start rather than after you need it.

**The dangerous window is short and specific:** the seconds during
step 4, while the new bootloader is being written. A power loss or a
disconnected cable there is what costs you a programmer. Before and
after, the board is fine.

**If you would rather not take that on,** you do not have to. Stay on
the original bootloader and skip to
[Staying on the old bootloader](#staying-on-the-old-bootloader).

Practical precautions, in order of how much they matter:

1. **Use a rear USB port on a desktop, or a powered hub.** Not a
   front-panel port, not an unpowered hub, not a laptop about to
   sleep.
2. **Do not use a USB-C dock or adapter** if you can avoid one. A dock
   renegotiating mid-write is a power loss.
3. **A laptop on battery is fine** — better than a desktop during a
   thunderstorm.
4. **Do not touch the board.** A nudged cable is the most likely way
   this goes wrong.

---

## Upgrading

### 1. Get the bootloader for your board

**Lakritz** — ECP5-25F:

```
$ curl -LO https://raw.githubusercontent.com/machdyne/lakritz/main/images/tinydfu_lakritz_25f_256k.bit
```

**Obst** — ECP5-12F:

```
$ curl -LO https://raw.githubusercontent.com/machdyne/obst/main/images/tinydfu_obst_256k.bit
```

Take the image built for **your board**. They are different FPGAs —
25F on Lakritz, 12F on Obst — so the wrong one will not configure at
all, and the pin assignments differ as well.

The `_256k` in the filename is the partition layout. An image without
it is the original 1 MB-user-partition bootloader and is not what you
want here.

### 2. Verify what you downloaded

```
$ sha256sum tinydfu_lakritz_25f_256k.bit    # or tinydfu_obst_256k.bit
```

Expected:

```
501c8fd3054d9e4f0456e547cabc6ccb3f8fd07a16c9ea86ec0f4a5d250501e1  tinydfu_lakritz_25f_256k.bit
33d95b8d15308adf222481cb7e0b7e2601061bf587c197d2418a719cfe38d4bb  tinydfu_obst_256k.bit
```

If a `SHA256SUMS` sits alongside the image in the repository, prefer
it — these values were correct when this document was written and the
images may have been rebuilt since:

```
$ sha256sum -c SHA256SUMS
```

Do not skip this. A truncated download that writes cleanly is exactly
the failure that costs you a programmer, and it is the one failure
this step eliminates entirely.

As a second check, the sizes are **125,170** bytes for Lakritz and
**125,265** for Obst. Anything much outside that is not a bootloader --
most likely a GitHub error page, which downloads as a perfectly valid
file of a few hundred KB and would brick the board if written.

### 3. Put the board in bootloader mode

Within the first 5 seconds of boot:

```
$ dfu-util -l
Found DFU: [16d0:116d] ... alt=2, name="Bootloader", ...
Found DFU: [16d0:116d] ... alt=1, name="User Data", ...
Found DFU: [16d0:116d] ... alt=0, name="User Image", ...
```

You need to see **`alt=2`**. If you do not, stop — writing to the
wrong alt setting is how this goes wrong.

On the original bootloader `alt=2` is named `Bootloader` and `alt=0`
is `User Image`, with no sizes. The updated one names them
`Boot Image (256KB)` and `User Image (1792KB)`, which is what makes
step 6 a one-glance check.

### 4. Write it

```
$ dfu-util -a 2 -D tinydfu_lakritz_25f_256k.bit
```

`dfu-util` will print `Warning: File has no DFU suffix`. That is
expected — a `.bit` is a raw bitstream with no suffix to check — and
it writes it correctly. It is also why step 5 can compare the readback
against the file directly.

Wait for it to finish. **Do not power-cycle yet.**

### 5. Verify before power-cycling

This is the step that makes the whole procedure survivable, and it is
the one people skip.

```
$ dfu-util -a 2 -U readback.bin
$ cmp readback.bin tinydfu_lakritz_25f_256k.bit
```

`cmp` printing nothing means the flash matches the file.

**If it does not match, do not power-cycle.** Go back to step 4 and
write it again. The board is still running the OLD bootloader from
RAM — the new one only takes effect on the next power cycle — so a bad
write can simply be repeated as many times as it takes.

`cmp` may report that `readback.bin` is longer than the image —
`dfu-util` reads back the whole 256 KB partition, and the bootloader
only occupies the first ~125 KB. That is fine. What matters is that
`cmp` reports no *differing byte*, only `EOF on ...`.

### 6. Power-cycle and confirm

Disconnect and within 5 seconds, check:

```
$ dfu-util -l
Found DFU: [16d0:116d] ... alt=0, name="User Image (1792KB)", ...
```

**`User Image (1792KB)`.** You are done.

### 7. Flash Zeitlos

```
$ dfu-util -a 0 -D zeitlos-lakritz_uart-dfu.bin
```

Then write the sdcard image and boot. See the release `README.txt` for
the sdcard.

---

## Recovery

If step 6 shows nothing and the board will not enumerate, the flash
holds a bootloader that does not run. The board is fine; the contents
are wrong.

The MMOD is a socketed SPI flash module, so any of these fixes it:

**JTAG**, if you have a cable — this is the easiest:

```
$ openFPGALoader -c dirtyJtag -f -o 0 tinydfu_lakritz_25f_256k.bit
```

**An MMOD programmer**, or any 3.3 V SPI flash programmer, with the
module out of its socket. [Werkzeug](https://github.com/machdyne/werkzeug) can do this, so can a Raspberry Pi Pico, Arduino, etc.

**A Raspberry Pi, ESP32 or similar** running `flashrom` over SPI.

The MMOD pinout is documented at https://github.com/machdyne/mmod.

In every case you are writing the same file to offset 0. Nothing about
the recovery is Zeitlos-specific — you are restoring the board to the
state it shipped in.

---

## If you would rather not upgrade the bootloader

You can still run Zeitlos. You cannot install it over USB.

**Flash over JTAG instead.** `zeitlos-<board>.img` writes the whole
2 MB and ignores partitions entirely, so it does not care what
bootloader is present — and it will overwrite it, which is fine if you
are flashing over JTAG anyway.

```
$ openFPGALoader -c dirtyJtag -f -o 0 zeitlos-lakritz_uart.img
```

Any 3.3 V SPI programmer or an MMOD programmer works too; the MMOD is
socketed.

**There is no reduced DFU image.** Zeitlos is 1303 KB and the original
user partition is 1024 KB. The parts that would have to come off are
the kernel and the applications, which is most of the system — what
would be left could not boot on its own, and moving the kernel out of
flash means moving the address the BIOS reads it from, which is a
permanent second boot path maintained forever for people who declined
a one-time upgrade. It is not worth it, so it does not exist.

If you have no JTAG cable and do not want to write the bootloader,
the honest answer is that this board cannot run Zeitlos yet.

## For the curious: why 256 KB

The bootloader is a USB device and an SPI flash writer — a few
thousand LUTs on a part that has 24,288. Compressed, it comes to
124,643 bytes on Lakritz and 126,079 on Obst. 256 KB is a little over
twice that, and it is a power of two on a part that erases in 4 KB
sectors.

Two things make the number worth thinking about rather than rounding.

**The user partition has to fit Zeitlos with room to spare.** Zeitlos
is 1303 KB today, and the pieces sit at fixed offsets — the boot
splash at `0x0F0000`, the kernel at `0x100000`, the applications at
`0x140000`. Those are the same offsets whether you flash over JTAG or
over USB, which is why upgrading the bootloader changes nothing about
Zeitlos itself. What moving `USERPART_START` does change is how much
room the gateware has before it runs into the splash:

```
  BOOTPART 256 KB   gateware gets 704 KB   uses 481 KB   46% spare
  BOOTPART 384 KB   gateware gets 576 KB   uses 481 KB   19% spare
  BOOTPART 512 KB   gateware gets 448 KB   uses 481 KB   does not fit
```

At 512 KB the gateware would collide with the splash and the layout
would have to be rearranged. At 256 KB nothing has to move at all.

**`ecppack --compress` is load-bearing.** An ECP5-25F bitstream is
7,468 frames of 696 bits — about **676 KB uncompressed**, whatever the
design does with the fabric, because every frame is written either
way. It is only the compression that makes a sparse design like the
bootloader small.

That matters if you ever build the bootloader yourself: **keep
`--compress`.** Dropping it produces a 676 KB image that overruns a
256 KB `BOOTPART` and corrupts the start of the user partition, which
is a much more confusing failure than a clean refusal.
