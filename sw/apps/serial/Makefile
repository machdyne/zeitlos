# sw/apps/serial -- build rules are shared: sw/common/app.mk, docs/build.md.

APP = serial

# Objects, and why each is linked:
#
#   zsoc.o: z_soc_has_feature2(), which z_uart1_present() is built on.
OBJS = zeitlos.o zobj.o zport.o zsoc.o zuart.o serial.o

include ../../common/app.mk
