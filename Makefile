CC=clang
OTHER_CFLAGS=
COMMON_LIBS=-lavformat -lavcodec -lavutil -lswscale -lc++

UNAME := $(shell uname)
LIBS_Darwin=${COMMON_LIBS} -framework CoreGraphics -framework CoreFoundation
LIBS_FreeBSD=${COMMON_LIBS} -lpng

DIRS_Darwin=-isystem /opt/local/include -L /opt/local/lib
DIRS_FreeBSD=-isystem /usr/local/include -L /usr/local/lib

.PHONY: clean

sophie: sophie.cpp output.cpp output.h input.cpp input.h util.h
	${CC} -o "$@" sophie.cpp output.cpp input.cpp --std=c++17 -Os -g ${OTHER_CFLAGS} ${DIRS_${UNAME}} ${LIBS_${UNAME}}

clean:
	rm sophie
