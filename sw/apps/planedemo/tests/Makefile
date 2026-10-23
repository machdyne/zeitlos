# sw/apps/planedemo/tests -- the film on a build host.
#
#   make wav      the music alone, out/planedemo.wav, through a model of
#                 the board's mixer
#   make video    the film: every other frame, then out/planedemo.mp4
#                 with the music (needs ffmpeg; the frames are kept in
#                 out/frames either way)
#   make sheet    one picture every two seconds, out/sheet.png
#   make slow     the film on a board much slower than the host: the
#                 jobs given a frame in 40 (the donut plays the frames it
#                 has), out/slow-sheet.png
#   make check    every frame queued and run at the boundary, and drawn
#                 as asked (PD_HOST_DIRECT=1): byte for byte the same

HOSTCC ?= cc
HOSTFLAGS = -std=gnu99 -Wall -Wextra -O2 -g -Wno-unused-parameter
COMMON = ../../../common
GAMES = $(COMMON)/games
MUSIC = ../pd_music.c ../pd_synth.c ../pd_song.c
RT = $(GAMES)/zarcade.c $(GAMES)/zsfx.c $(GAMES)/tests/ar_host.c $(COMMON)/zfont_data.c
FILM = ../planedemo.c ../pd_scenes.c ../pd_gfx.c ../pd_blob.c $(wildcard ../pd_s_*.c) ../../mausrun/sprites.c $(MUSIC)

all: wav

song_wav: song_wav.c $(MUSIC) ../pd_music.h
	$(HOSTCC) $(HOSTFLAGS) -DPD_HOST -o $@ song_wav.c $(MUSIC)

wav: song_wav
	mkdir -p out
	./song_wav out/planedemo.wav

# the film's data (pd_blob.c): what the board would make, made here
FILMDATA = ../pd_scenes.c ../pd_gfx.c $(wildcard ../pd_s_*.c) ../../mausrun/sprites.c $(MUSIC)
mkfilm: mkfilm.c $(FILMDATA) $(RT) $(wildcard ../*.h)
	$(HOSTCC) $(HOSTFLAGS) -DPD_HOST -DZSFX_HOSTED -DAR_HOST_TESTS -Dmain=game_main \
		-o $@ mkfilm.c ../planedemo.c $(FILMDATA) ../pd_blob.c $(RT)

film: mkfilm
	./mkfilm ../film.pdd

render_pd: render.c $(FILM) $(RT) $(wildcard ../*.h)
	$(HOSTCC) $(HOSTFLAGS) -DPD_HOST -DZSFX_HOSTED -DAR_HOST_TESTS -Dmain=game_main \
		-o $@ render.c $(FILM) $(RT)

frames: render_pd
	rm -rf out/frames && mkdir -p out/frames
	./render_pd out/frames 2 > out/render.log

video: frames wav
	ffmpeg -y -loglevel error -framerate 30 -i out/frames/f%05d.ppm -i out/planedemo.wav \
		-vf scale=640:480:flags=neighbor -c:v libx264 -pix_fmt yuv420p -crf 18 \
		-c:a aac -b:a 192k -shortest out/planedemo.mp4
	@echo "out/planedemo.mp4"

sheet: frames
	python3 sheet.py out/frames out/sheet.png

slow: render_pd
	rm -rf out/slow && mkdir -p out/slow
	PD_HOST_JOBS_EVERY=40 ./render_pd out/slow 2 > out/slow.log
	python3 sheet.py out/slow out/slow-sheet.png

clean:
	rm -rf song_wav render_pd mkfilm out

check: render_pd
	rm -rf out/check-q out/check-d && mkdir -p out/check-q out/check-d
	PD_FILM=../film.pdd ./render_pd out/check-q 3 > /dev/null
	PD_HOST_DIRECT=1 PD_FILM=../film.pdd ./render_pd out/check-d 3 > /dev/null
	@n=0; for f in out/check-q/*.ppm; do cmp -s $$f out/check-d/$${f##*/} || n=$$((n+1)); done; \
		echo "check: $$(ls out/check-q | wc -l) frames, $$n differ"; test $$n = 0

.PHONY: all wav frames video sheet slow check clean
