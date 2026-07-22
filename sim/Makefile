CC = gcc
CFLAGS = -O2 -Wall -Wextra -std=c11
SDL_CFLAGS = $(shell pkg-config --cflags sdl2)
SDL_LIBS = $(shell pkg-config --libs sdl2)

CORE_SRCS = machine.c cpu.c

all: zeitlos-sim zsim-headless zsim-debug

# The end-user tool: ./zeitlos-sim app.bin
zeitlos-sim: main_sdl.c $(CORE_SRCS) machine.h cpu.h
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -o $@ $(CORE_SRCS) main_sdl.c $(SDL_LIBS)

# Headless variant: no display needed, dumps the framebuffer to PBM files.
# Useful for CI / testing without a display server.
zsim-headless: main_headless.c $(CORE_SRCS) machine.h cpu.h
	$(CC) $(CFLAGS) -o $@ $(CORE_SRCS) main_headless.c

# Single-instruction-step trace tool, for debugging boot/early-crash issues.
zsim-debug: main_debug.c $(CORE_SRCS) machine.h cpu.h
	$(CC) $(CFLAGS) -o $@ $(CORE_SRCS) main_debug.c

clean:
	rm -f zeitlos-sim zsim-headless zsim-debug

.PHONY: all clean
