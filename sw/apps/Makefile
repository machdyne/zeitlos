# PREFIX is optional; forwarded so a multi-lib gcc can be used
# (make PREFIX=/opt/riscv/bin/riscv64-unknown-elf-).
APP_MAKE = $(MAKE) $(if $(PREFIX),PREFIX=$(PREFIX),)

all:
	cd blinky && $(APP_MAKE)
	cd hello && $(APP_MAKE)
	cd gpudemo && $(APP_MAKE)
	cd gpu3d && $(APP_MAKE)
	cd bounce && $(APP_MAKE)
	cd bounceblit && $(APP_MAKE)
	cd ping && $(APP_MAKE)
	cd pong && $(APP_MAKE)
	cd wm && $(APP_MAKE)
	cd hello_win && $(APP_MAKE)
	cd net && $(APP_MAKE)
	cd term && $(APP_MAKE)
	cd portdemo && $(APP_MAKE)
	cd repl && $(APP_MAKE)

clean:
	cd blinky && $(APP_MAKE) clean
	cd hello && $(APP_MAKE) clean
	cd gpudemo && $(APP_MAKE) clean
	cd gpu3d && $(APP_MAKE) clean
	cd bounce && $(APP_MAKE) clean
	cd bounceblit && $(APP_MAKE) clean
	cd ping && $(APP_MAKE) clean
	cd pong && $(APP_MAKE) clean
	cd wm && $(APP_MAKE) clean
	cd hello_win && $(APP_MAKE) clean
	cd net && $(APP_MAKE) clean
	cd term && $(APP_MAKE) clean
	cd portdemo && $(APP_MAKE) clean
	cd repl && $(APP_MAKE) clean
