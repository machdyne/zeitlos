# sw/apps/usb -- the USB host stack as a driver process (docs/usb_driver.md).
# Build rules are shared: sw/common/app.mk, docs/build.md.

APP = usb

# A core app in the flash archive: integer-only printf.
ZFMT = 1

# A driver (ZEXE flags bit 15, docs/userspace_drivers.md "Drivers"): it
# drives the USB host controller and takes IRQ 9 with z_irq_bind().
APP_DRIVER = 1

# No large buffers on the stack: requests and replies live in .bss.
APP_STACK = 16K

# The bring-up tools (usbcap, usbtune, usbidle, usbbuf, lsusb's detail,
# the storage driver's verbose messages): `make USBH_DEBUG=1`. They cost
# flash in the archive, not kernel space, since Phase 3.
# printf, puts and putchar are this app's own (usb.c; usbh.h renames
# them under Z_USBH_OWN_PRINTF): zfmt's vsnprintf, then write(). The
# printf that ZFMT gives an app goes through newlib's stdio, whose
# buffers bring malloc -- about 12KB of flash for the few lines the
# stack prints.
APP_CFLAGS += -DZ_USBH_OWN_PRINTF -DZFMT_NO_STDIO

USBH_DEBUG ?= 0
ifeq ($(USBH_DEBUG), 1)
APP_CFLAGS += -DUSBH_DEBUG
endif

OBJS = zeitlos.o zobj.o usbh.o usbh_hid.o usbh_hub.o usbh_msc.o \
	usbh_cdc.o usbh_cp210x.o usbh_ecm.o usb.o

include ../../common/app.mk
