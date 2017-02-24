
CC=gcc
CFLAGS=-std=c99 -g -O3 -march=native -funroll-loops -ffast-math \
       # -ftree-vectorize \
       # -ftree-vectorizer-verbose=6 \
       # -fopt-info-vec-missed

all: _tsinfer.cpython-34m.so 

_tsinfer.cpython-34m.so: _tsinfermodule.c 
	CFLAGS="${CFLAGS}" python3 setup.py build_ext --inplace

ctags:
	ctags *.c *.h

clean:
	rm -f *.so *.o tags
	rm -fR build