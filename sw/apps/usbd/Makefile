# sw/apps/usbd -- the USB-C socket as a USB device: the console and USB
# ethernet (docs/usb_dev.md). Build rules are shared: sw/common/app.mk,
# docs/build.md.

APP = usbd

# A driver (ZEXE flags bit 15, docs/userspace_drivers.md "Drivers"): it
# serves rtl/usbd_ep.v's endpoints 0-3 and takes IRQ 11 with
# z_irq_bind().
APP_DRIVER = 1

OBJS = zeitlos.o usbd.o

include ../../common/app.mk
