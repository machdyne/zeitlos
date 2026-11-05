# zgl's pages against a model of T2 and the scanout (model.c)
all: model
	./model 0
	./model 1
chunky_x.c: ../../zgl_chunky.c mk.py
	python3 mk.py ../../zgl_chunky.c chunky_x.c
model: model.c chunky_x.c stubs.h
	cc -O1 -g -Wall -Wno-unused-function -I. -o $@ model.c chunky_x.c
clean:
	rm -f model chunky_x.c
