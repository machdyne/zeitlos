# sw/apps/netfake -- build rules are shared: sw/common/app.mk, docs/build.md.
#
# A stand-in for `net` in the emulator (zsys has no network): it takes
# the name net0 and answers a socket's CONNECT with a little web server
# of its own, serving the card's /media/video over HTTP with ranges. For
# testing cinema's films from a web server (docs/cinema_app.md) without
# a board. Not in sw/apps/Makefile's APPS and on no card.

APP = netfake

OBJS = zeitlos.o zobj.o zport.o zfsapp.o netfake.o

POSIX_IO = 1
APP_STACK = 64K

include ../../common/app.mk
