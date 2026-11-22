# sw/apps/share -- build rules are shared: sw/common/app.mk, docs/build.md.
#
# The SD card as a drive on the PC (docs/usb_dev.md, "The card on the
# PC"). A driver: it runs the USB device (IRQ 11, rtl/usbd_ep.v) and the
# card's SPI master while fs has let go of the card. Its device is
# sw/apps/usbd/usbd.c with the card reader (-DUSBD_MSC) and without its
# main(); the card's driver is fs's own (sw/apps/fs/fatfs/sdmm.c).

APP = share
APP_DRIVER = 1
APP_CFLAGS = -DUSBD_MSC -DUSBD_NO_MAIN -I../fs/fatfs -I../../common
SRC_DIRS = ../usbd ../fs/fatfs

OBJS = zeitlos.o zobj.o zfsc.o zgfx.o zfont_data.o zwin.o zwidget.o zspeak.o \
	usbd.o sdmm.o xlcard.o share.o

include ../../common/app.mk
