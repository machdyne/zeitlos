# planedemo -- the film's instructions counted on a RISC-V emulator
# (unicorn, pip install unicorn pyelftools): what each scene's frames
# and the work behind them cost the board's CPU. make; make run
CC = riscv-none-elf-gcc
P = ../..
G = ../../../../common/games
C = ../../../../common
CFLAGS = -march=rv32im -mabi=ilp32 -Os -ffunction-sections -fdata-sections \
	-DPD_HOST -DPD_MEASURE -DZSFX_HOSTED -DAR_HOST_TESTS -Dmain=game_main -w
SRC = $(P)/planedemo.c $(P)/pd_scenes.c $(P)/pd_gfx.c $(wildcard $(P)/pd_s_*.c) \
	$(P)/pd_music.c $(P)/pd_synth.c $(P)/pd_song.c ../../../mausrun/sprites.c \
	$(G)/zarcade.c $(G)/zsfx.c $(C)/zfont_data.c

film.elf: $(SRC) rvstub.c crt.S link.ld ../../film.pdd
	$(CC) $(CFLAGS) -c rvstub.c -o rvstub.o -Umain
	cd ../.. && $(CC) -march=rv32im -mabi=ilp32 -c pd_blob.c -o tests/rv/pd_blob.o
	$(CC) $(CFLAGS) -o $@ -nostartfiles -T link.ld -Wl,--no-relax -Wl,--gc-sections crt.S $(SRC) rvstub.o pd_blob.o \
		-specs=nano.specs -specs=nosys.specs -lgcc

run: film.elf
	python3 run.py film.elf

clean:
	rm -f film.elf rvstub.o pd_blob.o
